# Wuffs Streaming Parsers

## 1. Motivation

AI inference and agentic request bodies impose unusual memory requirements on a
proxy. A single `POST /v1/chat/completions` may carry dozens of conversation
turns, each potentially containing base64-encoded images or large tool schemas,
pushing the JSON body into the hundreds of kilobytes. A JSON-RPC `tools/call`
body may carry a large `arguments` object whose values are never needed for
routing. Under concurrent load these bodies must be parsed with strict, predictable
heap cost.

### The `token_buf_` vulnerability in `IncrementalJsonTokenizer`

The original `AgentBodyParser` used `IncrementalJsonTokenizer` — a bespoke
14-state machine — to parse JSON-RPC request bodies. The tokenizer maintained a
single `std::string token_buf_` that accumulated every JSON key it encountered at
every nesting depth. In Tier 2 semantic mode (body too large for `params` capture),
the tokenizer still had to walk the entire body to extract routing fields, and
`token_buf_` grew with every key regardless of its depth.

An attacker-crafted body like:

```json
{
  "method": "tools/call",
  "params": {
    "AAAAAAAAAAAAA...4 MB of A's...AAAAAAAA": "value"
  }
}
```

caused `token_buf_` to grow proportionally to the key inside `params`. The Tier 2
O(1) heap guarantee was defeated — the attacker controlled heap allocation by
choosing a long key name inside a nested object. The tokenizer accumulated keys at
every depth without distinction between depth-1 keys (which must be accumulated for
routing) and depth-2+ keys (which are irrelevant and should be discarded in Tier 2).

`InferenceBodyParser` had the same vulnerability. It too was backed by
`IncrementalJsonTokenizer`, which accumulated every JSON key and number literal
into `token_buf_`, including keys inside `messages[]` elements at depth 3 and
beyond, before firing any callback. An attacker embedding a long key name anywhere
inside a `messages[]` element would cause `token_buf_` to grow proportionally — even
in Tier 2 where element content provides no routing signal and should be discarded
at zero heap cost. Wuffs eliminates this for both parsers.

### Why Wuffs

Wuffs (Wrangling Unsafe File Formats Safely) is a memory-safe, formally-verified
parser toolkit. Its JSON decoder is a stackless coroutine: all parse state lives in
a fixed-size heap struct (`wuffs_json__decoder`, approximately 2 KB), not on the
C++ call stack. This is exactly the property needed for streaming HTTP body parsing:

- **No `token_buf_` equivalent**: Wuffs does not accumulate a string for a token
  before emitting it. Each token arrives with a length field (`tlen`, 16-bit,
  max 65535 bytes) and a raw pointer into the source buffer. The application reads
  the bytes it needs in place and discards the token.
- **Bounded-per-token memory**: regardless of key or value length, the application
  sees one token of at most 65535 bytes at a time. A 4 MB key produces approximately
  62 tokens; each is processed in O(1) and discarded.
- **Stackless coroutine**: the same `dec_` pointer resumes exactly where it stopped
  across `feed()` calls. No C++ call-stack state needs to be preserved between HTTP
  chunks.
- **Formally verified**: the Wuffs toolchain proves memory safety at compile time.
- **No nlohmann**: routing fields are extracted inline during the streaming Wuffs
  scan. There is no second-pass DOM parse at `finish()` time.

---

## 2. Wuffs token model

Every call to `wuffs_json__decoder__decode_tokens()` fills `tok_buf_` with as
many tokens as fit in the 256-slot ring, then returns a status. Tokens are consumed
in the inner loop before the next decode call. Four token classes matter for both
parsers:

| VBC constant | Meaning | Action |
|---|---|---|
| `FILLER` | Whitespace, commas, colons | Advance `body_src_pos_` by `tlen`; no other action |
| `STRUCTURE` | Object/array open or close | Manage `depth_`, `is_dict_[]`, `expecting_key_[]`; record byte ranges |
| `STRING` | String content, quotes, or escapes | Gate on `str_target_`; call `appendStringToken` when non-null |
| `NUMBER` or `LITERAL` | Numeric or `true`/`false`/`null` | Extract scalars if applicable; advance `expecting_key_` |

### The `continued` flag and multi-token strings

A single JSON string may span multiple Wuffs tokens when it is longer than 65535
bytes or contains escape sequences that force a token boundary. The `continued`
flag (`cont`) is `true` on all tokens except the last of a chain. Both parsers
track chain state with `in_chain_`:

- **First token** (`!in_chain_`): clear `str_acc_`, determine `string_is_key_`,
  set `str_target_`.
- **Continuation tokens**: append to `*str_target_` (if non-null) via
  `appendStringToken`.
- **Final token** (`!cont`): commit the completed string; reset `str_target_ = nullptr`.

`in_chain_` is a data member, not a local variable, so in-progress string state
survives across `feed()` call boundaries when Wuffs returns `short_read` mid-chain.

### STRING DROP vs COPY tokens

String tokens carry VBD flags that distinguish three sub-types:

| VBD flag | Meaning | `appendStringToken` action |
|---|---|---|
| `CONVERT_0_DST_1_SRC_DROP` | Opening/closing quote or escape introducer | Early return — advance `body_src_pos_` but produce zero output bytes |
| `CONVERT_1_DST_1_SRC_COPY` | Plain ASCII or UTF-8 bytes | `out.append(raw)` directly |
| Neither flag | JSON escape sequence (`\n`, `\uXXXX`, etc.) | Decode inline; append decoded character(s) |

The DROP check is the first thing `appendStringToken` does:

```cpp
if (vbd & WUFFS_BASE__TOKEN__VBD__STRING__CONVERT_0_DST_1_SRC_DROP) return;
```

Failing to check it would append literal `"` and `\` characters into accumulated
routing fields and corrupt all subsequent comparisons.

### Short-read vs short-write

After draining the inner token loop, `feedChunk` inspects the status from
`decode_tokens`:

```
status == nullptr           → parse complete (OK); wuffs_done_ = true, break outer
is_note(&status)            → end_of_data on non-closed stream; wuffs_done_ = true, break
status == short_read        → need more input; break outer, return OK (wait for next feed)
status == short_write       → tok_buf_ full mid-stream; reset ri/wi to 0, continue outer
status is error             → invalid JSON; return InvalidArgumentError
```

`short_write` is the case where Wuffs produced 256 tokens before exhausting the
source. The outer loop resets `tok_buf_.meta.ri = tok_buf_.meta.wi = 0` and calls
`decode_tokens` again without advancing `src_buf` — Wuffs resumes from where it
left off in the source because source state is managed inside the coroutine.

---

## 3. Shared infrastructure

### `appendStringToken` free function

`appendStringToken` is defined as a free function in an anonymous namespace at the
top of `request_decoder.cc`, before either parser class, and is shared by both
`InferenceBodyParser` and `AgentBodyParser`:

```cpp
namespace {
void appendStringToken(std::string& out, absl::string_view raw, uint64_t vbd) {
  if (vbd & WUFFS_BASE__TOKEN__VBD__STRING__CONVERT_0_DST_1_SRC_DROP) return;
  if (vbd & WUFFS_BASE__TOKEN__VBD__STRING__CONVERT_1_DST_1_SRC_COPY) {
    out.append(raw.data(), raw.size());
    return;
  }
  // ... inline escape decoding for \n, \t, \uXXXX, etc.
}
} // namespace
```

Both parsers call this function from the same site within their STRING token
handlers, passing `*str_target_` (when non-null). The function handles all three
string sub-types (DROP, COPY, escape sequences) so neither parser needs escape
decoding logic of its own.

### Universal `feedChunk` loop skeleton

The outer loop structure is identical in both parsers:

```
feedChunk(chunk, closed):
  if !dec_: return InternalError
  if wuffs_done_: return OK          ← early exit, prevents re-entry

  chunk_base = body_src_pos_         ← absolute body offset of chunk[0]
  src_buf = wuffs_base__ptr_u8__reader(chunk, closed)

  outer loop:
    status = decode_tokens(dec_, &tok_buf_, &src_buf, empty_slice)

    inner loop — drain tok_buf_:
      tok  = tok_buf_.data.ptr[tok_buf_.meta.ri++]
      vbc  = token_value_base_category(tok)
      vbd  = token_value_base_detail(tok)
      tlen = token_length(tok)
      cont = token_continued(tok)

      tok_start      = body_src_pos_
      body_src_pos_ += tlen           ← advance for EVERY token

      switch vbc: ...parser-specific handling...

      if has_error_: break inner loop

    if has_error_: return InvalidArgumentError

    inspect status:
      nullptr     → wuffs_done_=true, break outer
      is_note     → wuffs_done_=true, break outer
      short_read  → break outer (return OK — wait for next feed())
      short_write → tok_buf_.meta.ri=wi=0, continue outer
      error       → return InvalidArgumentError

  return OkStatus
```

The only differences between the two parsers are in the `switch vbc` body.

### `body_src_pos_` invariant

`body_src_pos_` advances by `tlen` for **every** token Wuffs produces — including
FILLER (whitespace, colons, commas) and STRING DROP tokens (opening and closing
quotes). It is always synchronized with the raw source byte position at the start
of each token. Combined with `chunk_base` (the value of `body_src_pos_` at the
beginning of each `feedChunk` call), this makes raw byte extraction exact:

```cpp
const size_t tok_start = body_src_pos_;
body_src_pos_ += tlen;
absl::string_view raw = chunk.substr(tok_start - chunk_base, tlen);
```

The byte positions recorded for element ranges, `params_byte_start_/end_`,
`arguments_byte_start_/end_`, etc. are always true offsets into the raw body
stream that `residual_writer_` has captured. `makeSubRef` can safely index into
`residual_params` using these offsets.

---

## 4. InferenceBodyParser

`InferenceBodyParser` is a private inner class of `RequestDecoder`, defined in
`request_decoder.cc` (lines 81–400). It is constructed when request headers
classify the incoming request as an OpenAI inference body
(`POST /v1/chat/completions`, `/v1/completions`, `/v1/embeddings`, etc.).

### 4.1 Class overview

```
RequestDecoder
  └─ InferenceBodyParser
       ├─ Wuffs state
       │    wuffs_json__decoder::unique_ptr  dec_              ← stackless coroutine (~2 KB)
       │    wuffs_base__token  tok_data_[256]                  ← 2048-byte token ring (in-object)
       │    wuffs_base__token_buffer  tok_buf_                 ← slice wrapper over tok_data_
       │    size_t  body_src_pos_                              ← monotonic body byte counter
       │    bool  wuffs_done_                                  ← EOF/complete sentinel
       │
       ├─ residual_writer_: std::unique_ptr<StreamWriter>      ← full body captured (lazy open)
       │
       ├─ Depth/structure
       │    int  depth_                                        ← current nesting depth
       │    bool  is_dict_[8]                                  ← is container at depth a dict?
       │    bool  expecting_key_[8]                            ← is dict at depth expecting key?
       │
       ├─ String accumulation
       │    bool  in_chain_                                    ← inside a multi-token string?
       │    bool  string_is_key_                               ← current string is a dict key?
       │    std::string  str_acc_                              ← key/value accumulator
       │    std::string* str_target_                           ← where to write current string
       │    std::string  string_val_                           ← scratch for stop strings
       │
       ├─ Depth-1 extracted fields
       │    std::string  current_key_                          ← most recently completed key
       │    std::string  model_
       │    bool  streaming_
       │    SamplingParams  sampling_
       │
       ├─ Container tracking
       │    bool  in_messages_                                 ← inside messages[] array
       │    bool  in_tools_                                    ← inside tools[] array
       │    bool  in_stop_array_                               ← inside stop[] array
       │    bool  in_elem_                                     ← inside a messages/tools element
       │    bool  elem_is_dict_                                ← element is object (vs array)
       │    size_t  elem_start_                                ← body offset of element open
       │
       ├─ Element byte ranges (Tier 1 only)
       │    std::vector<pair<size_t,size_t>>  message_ranges_  ← {start, end} per element
       │    std::vector<PayloadKind>           message_kinds_
       │    std::vector<pair<size_t,size_t>>  tool_ranges_
       │    std::vector<PayloadKind>           tool_kinds_
       │
       ├─ Duplicate-key guards
       │    bool  seen_model_, seen_stream_, seen_messages_, seen_tools_
       │    bool  seen_temperature_, seen_top_p_, seen_max_tokens_, seen_n_
       │    bool  seen_seed_, seen_stop_
       │
       └─ Error state
            bool  has_error_; std::string  error_
```

### 4.2 Lifecycle

```
InferenceBodyParser constructed:
  dec_     = wuffs_json__decoder::alloc()        ← ~2 KB heap alloc, one-time
  tok_buf_ = wuffs_base__slice_token__writer(tok_data_, 256)
  residual_writer_ = nullptr                     ← opened lazily on first feed()

feed(chunk):
  total_bytes_ += chunk.size()
  if total_bytes_ > max_body_bytes: return ResourceExhausted    ← Tier 3 hard reject
  if !residual_writer_: residual_writer_ = store_.beginStore(JsonObject)
  residual_writer_->append(chunk)                ← stream raw bytes to PayloadStore
  return feedChunk(chunk, /*closed=*/false)

finish(payload, request):
  feedChunk("", /*closed=*/true)                 ← signal EOF to Wuffs coroutine
  payload.target.name = std::move(model_)
  payload.sampling    = std::move(sampling_)
  request.streaming   = streaming_
  payload.residual_params = residual_writer_->finalize()
  for each message_ranges_[i]: makeSubRef → payload.messages.push_back(ref)
  for each tool_ranges_[i]:    makeSubRef → payload.tools.push_back(ref)
```

### 4.3 STRUCTURE token handling

The STRUCTURE case manages depth state and records byte ranges for element
extraction.

**PUSH** (entering a container):

- Increment `depth_`, set `is_dict_[depth_]` and `expecting_key_[depth_]`.
- At depth 2, gate on `current_key_`:
  - `"messages"` → `in_messages_=true`, `in_tools_=false`
  - `"tools"` → `in_tools_=true`, `in_messages_=false`
  - `"stop"` → `in_stop_array_=true`
- At depth 3, when `in_messages_` or `in_tools_` and `captureEnabled()`:
  - `in_elem_=true`, `elem_start_=tok_start`, `elem_is_dict_=to_dict`

**POP** (closing a container):

- At depth 3, when `in_elem_`:
  - `in_elem_=false`
  - Record `{elem_start_, body_src_pos_}` into `message_ranges_` or `tool_ranges_`
    (with `body_src_pos_` already advanced past the closing delimiter).
- At depth 2 (any key):
  - `in_messages_=false`, `in_tools_=false`, `in_stop_array_=false`
- Restore `expecting_key_[depth_]` for the parent dict.

Note that the byte-range recording at depth 3 happens unconditionally for all
containers when `captureEnabled()` is true — whether `in_messages_` or `in_tools_`.
Only the target vector (`message_ranges_` vs `tool_ranges_`) differs.

### 4.4 `str_target_` gating rules

The central memory-safety mechanism is `str_target_`: a pointer to the `std::string`
that should receive the decoded bytes of the current string token chain, or `nullptr`
to discard.

```
if string_is_key_:
    str_target_ = &str_acc_           ← always accumulate keys for routing decisions

else if depth_ == 1:
    current_key_ == "model"  → &model_
    current_key_ == "stop"   → &string_val_    (single stop string at depth 1)
    anything else            → nullptr          (discarded — e.g. "jsonrpc" value)

else if depth_ == 2 && in_stop_array_:
    str_target_ = &string_val_        (array stop string element)

else:
    nullptr                            (discarded unconditionally — depth 3+ values: 0 bytes)
```

When a stop string chain completes (`str_target_ == &string_val_`), the completed
string is moved into `sampling_.stop`:

```cpp
if (str_target_ == &string_val_) {
    sampling_.stop.push_back(std::move(string_val_));
    string_val_.clear();
}
```

The key property of depth 3+ values: `str_target_` is `nullptr`, so
`appendStringToken` is never called. The raw bytes exist only in the HTTP chunk
buffer (a `string_view` into Envoy's existing data frame); no heap allocation
occurs for that content regardless of its length.

### 4.5 Scalar extraction

**NUMBER tokens at depth 1**: the raw token bytes are read in place from the chunk
and parsed with `absl::SimpleAtoi` or `absl::SimpleAtod`:

| `current_key_` | Parse | Destination |
|---|---|---|
| `"max_tokens"` | `SimpleAtoi` → `int32_t` | `sampling_.max_tokens` |
| `"n"` | `SimpleAtoi` → `int32_t` | `sampling_.n` |
| `"seed"` | `SimpleAtoi` → `int64_t` | `sampling_.seed` |
| `"temperature"` | `SimpleAtod` → `double` | `sampling_.temperature` |
| `"top_p"` | `SimpleAtod` → `double` | `sampling_.top_p` |

**LITERAL tokens at depth 1**: the raw bytes are compared inline to `"true"` and
`"false"`. When `current_key_ == "stream"`, the result is stored in `streaming_`.
No heap allocation occurs.

### 4.6 Body-size tiering

`DecoderConfig` exposes two thresholds that divide the body-size space into three
tiers.

```
┌────────────────────────────────────────────────────────────────────────────┐
│ body size                   │ tier │ behavior                              │
├────────────────────────────────────────────────────────────────────────────┤
│ ≤ max_element_capture_bytes │  1   │ Full capture. At depth 3 push inside  │
│   (default 256 KB)          │      │ messages[]/tools[], in_elem_=true     │
│                             │      │ and elem_start_ records the opening   │
│                             │      │ delimiter. At depth 3 pop, the        │
│                             │      │ {start, end} range is recorded.       │
│                             │      │ finish() converts ranges to           │
│                             │      │ PayloadRef sub-refs of residual.      │
├────────────────────────────────────────────────────────────────────────────┤
│ > max_element_capture_bytes │  2   │ Semantic-only. in_elem_ is never set; │
│ ≤ max_body_bytes (4 MB)     │      │ message_ranges_ and tool_ranges_ are  │
│                             │      │ empty. payload.messages and           │
│                             │      │ payload.tools remain empty. Scalar    │
│                             │      │ extraction (model_, streaming_,       │
│                             │      │ sampling_) runs identically.          │
├────────────────────────────────────────────────────────────────────────────┤
│ > max_body_bytes            │  3   │ Hard reject. feed() returns           │
│                             │      │ ResourceExhausted immediately.        │
└────────────────────────────────────────────────────────────────────────────┘
```

**Unified code path**: there is no separate handler or mode switch between Tier 1
and Tier 2. The only branch is the `captureEnabled()` check inside the STRUCTURE
PUSH at depth 3:

```cpp
if (depth_ == 3 && (in_messages_ || in_tools_) && captureEnabled()) {
    in_elem_      = true;
    elem_start_   = tok_start;
    elem_is_dict_ = to_dict;
}
```

`captureEnabled()` is:

```cpp
bool captureEnabled() const {
    return total_bytes_ <= config_.max_element_capture_bytes;
}
```

When `captureEnabled()` returns `false`, `in_elem_` is never set. In the depth-3
POP case, `if (pop_depth == 3 && in_elem_)` is false, so no range is recorded.
All other behavior — scalar extraction, `body_src_pos_` tracking, residual
streaming — runs identically in both tiers.

### 4.7 E2E trace — chat/completions (Tier 1)

**Request body:**

```json
{"model":"gpt-4o","stream":true,"max_tokens":512,"messages":[{"role":"user","content":"Hi"}]}
```

Body size: approximately 90 bytes. Well under 256 KB → Tier 1.
`captureEnabled()` returns `true` throughout.

**Token-by-token trace** (`body_src_pos_` starts at 0, `chunk_base = 0`):

```
STRUCTURE PUSH to_dict=true  ← opening '{' of root object
  depth_=1, is_dict_[1]=true, expecting_key_[1]=true

STRING DROP '"', COPY "model", DROP '"'
  first_in_group=true, string_is_key_=true (is_dict_[1] && expecting_key_[1])
  str_target_=&str_acc_
  DROP: no-op. COPY: str_acc_="model". DROP: chain complete.
  !cont → depth_==1: seen_model_=true; current_key_="model"; expecting_key_[1]=false

FILLER ":"

STRING DROP '"', COPY "gpt-4o", DROP '"'
  first_in_group=true, string_is_key_=false, depth_==1, current_key_=="model"
    → model_.clear(); str_target_=&model_
  COPY: model_="gpt-4o"
  chain complete: is_dict_[1] → expecting_key_[1]=true

STRING COPY "stream" (key)
  current_key_="stream", seen_stream_=true; expecting_key_[1]=false

LITERAL "true"
  depth_==1, current_key_=="stream" → streaming_=true
  is_dict_[1] → expecting_key_[1]=true

STRING COPY "max_tokens" (key)
  current_key_="max_tokens", seen_max_tokens_=true; expecting_key_[1]=false

NUMBER "512"
  depth_==1, current_key_=="max_tokens"
  SimpleAtoi → sampling_.max_tokens=512
  is_dict_[1] → expecting_key_[1]=true

STRING COPY "messages" (key)
  current_key_="messages", seen_messages_=true; expecting_key_[1]=false

STRUCTURE PUSH to_dict=false  ← opening '[' of messages array
  depth_=2, is_dict_[2]=false, expecting_key_[2]=false
  current_key_=="messages": in_messages_=true, in_tools_=false

STRUCTURE PUSH to_dict=true  ← opening '{' of messages[0]
  depth_=3, is_dict_[3]=true, expecting_key_[3]=true
  in_messages_=true && captureEnabled()=true:
    in_elem_=true, elem_start_=tok_start (offset of '{'), elem_is_dict_=true

STRING COPY "role" (key at depth 3)
  string_is_key_=true → str_target_=&str_acc_
  str_acc_="role"
  chain complete: depth_==3 (not 1, not 2&&in_params_) → expecting_key_[3]=false

STRING COPY "user" (value at depth 3)
  string_is_key_=false, depth_==3
    → str_target_=nullptr    ← depth 3 value: discarded, 0 heap
  chain complete: is_dict_[3] → expecting_key_[3]=true

STRING COPY "content" (key at depth 3)
  str_acc_="content"; expecting_key_[3]=false

STRING COPY "Hi" (value at depth 3)
  str_target_=nullptr    ← discarded, 0 heap
  expecting_key_[3]=true

STRUCTURE POP  ← closing '}' of messages[0]
  pop_depth=3, depth_=2
  in_elem_=true:
    in_elem_=false
    in_messages_=true → message_ranges_.push_back({elem_start_, body_src_pos_})
    message_kinds_.push_back(JsonObject)
  is_dict_[2]=false → no expecting_key_ update

STRUCTURE POP  ← closing ']' of messages array
  pop_depth=2, depth_=1
  in_messages_=false, in_tools_=false, in_stop_array_=false
  is_dict_[1]=true → expecting_key_[1]=true

STRUCTURE POP  ← closing '}' of root object
  pop_depth=1, depth_=0

decode_tokens returns status==nullptr (OK)
  wuffs_done_=true, break outer
```

**finish():**

```
feedChunk("", closed=true)
  wuffs_done_=true → return OK immediately

payload.target.name = "gpt-4o"
payload.sampling.max_tokens = 512
request.streaming = true
payload.residual_params = residual_writer_->finalize()
  → PayloadRef::External{offset=0, len=90}  (full body in mmap arena)

message_ranges_[0] = {elem_start, body_src_pos_at_pop}
makeSubRef(payload.messages[0], elem_start, len, residual, JsonObject)
  → PayloadRef::External{mmap_offset + elem_start, len}  (no copy)

tool_ranges_ is empty → payload.tools is empty
```

---

## 5. AgentBodyParser

`AgentBodyParser` is a private inner class of `RequestDecoder`, defined in
`request_decoder.cc` (lines 416–769). It is constructed when request headers
classify an incoming request as an agent (MCP or A2A) JSON-RPC body.

### 5.1 Class overview

```
RequestDecoder
  └─ AgentBodyParser
       ├─ Wuffs state
       │    wuffs_json__decoder::unique_ptr  dec_              ← stackless coroutine (~2 KB)
       │    wuffs_base__token  tok_data_[256]                  ← 2048-byte token ring (in-object)
       │    wuffs_base__token_buffer  tok_buf_                 ← slice wrapper over tok_data_
       │    size_t  body_src_pos_                              ← monotonic body byte counter
       │    bool  wuffs_done_                                  ← EOF/complete sentinel
       │
       ├─ residual_writer_: std::unique_ptr<StreamWriter>      ← full body captured (lazy open)
       │
       ├─ Depth/structure
       │    int  depth_                                        ← current nesting depth
       │    bool  is_dict_[8]                                  ← is container at depth a dict?
       │    bool  expecting_key_[8]                            ← is dict at depth expecting key?
       │
       ├─ String accumulation
       │    bool  in_chain_                                    ← inside a multi-token string?
       │    bool  string_is_key_                               ← current string is a dict key?
       │    std::string  str_acc_                              ← key accumulator
       │    std::string* str_target_                           ← where to write current string
       │
       ├─ Depth-1 extracted fields
       │    std::string  current_key_                          ← most recently completed key
       │    std::string  id_                                   ← jsonrpc id
       │    std::string  method_                               ← rpc method name
       │
       ├─ params container tracking
       │    bool   in_params_                                  ← inside params container
       │    size_t params_byte_start_                          ← offset of params open delimiter
       │    size_t params_byte_end_                            ← offset one past params close delimiter
       │
       ├─ params sub-field extraction
       │    std::string  params_key_                           ← most recently completed params key
       │    std::string  params_name_                          ← "name" field inside params
       │    std::string  params_uri_                           ← "uri" field inside params
       │    std::string  params_ref_                           ← "ref" field inside params
       │
       ├─ sub-container (arguments / capabilities) tracking
       │    bool        in_sub_container_                      ← inside arguments or capabilities
       │    bool        sub_is_arguments_                      ← true if arguments, false if capabilities
       │    size_t      sub_container_start_                   ← offset of sub-container open
       │    size_t      arguments_byte_start_, arguments_byte_end_
       │    size_t      capabilities_byte_start_, capabilities_byte_end_
       │    PayloadKind arguments_kind_                        ← JsonObject or JsonArray
       │
       ├─ Duplicate-key guards
       │    bool  seen_jsonrpc_, seen_id_, seen_method_, seen_params_
       │
       └─ Error state
            bool  has_error_; std::string  error_
```

### 5.2 Lifecycle

```
AgentBodyParser constructed:
  dec_     = wuffs_json__decoder::alloc()        ← ~2 KB heap alloc, one-time
  tok_buf_ = wuffs_base__slice_token__writer(tok_data_, 256)
  residual_writer_ = nullptr                     ← opened lazily on first feed()

feed(chunk):
  total_bytes_ += chunk.size()
  if total_bytes_ > max_body_bytes: return ResourceExhausted    ← Tier 3 hard reject
  if !residual_writer_: residual_writer_ = store_.beginStore(JsonObject)
  residual_writer_->append(chunk)                ← stream raw bytes to PayloadStore
  return feedChunk(chunk, /*closed=*/false)

finish(payload, request):
  feedChunk("", /*closed=*/true)                 ← signal EOF to Wuffs coroutine
  request.jsonrpc_id = std::move(id_)
  request.rpc_method = std::move(method_)
  classify({http_method_, path_, headers_, rpc_method}) → payload.invocation / dialect
  populatePayload(payload)                       ← route params_name_/uri_/ref_
  payload.residual_params = residual_writer_->finalize()
  makeSubRef(payload.params_raw, ...)            ← sub-range of residual_params
  makeSubRef(payload.arguments, ...)             ← sub-range of residual_params (Tier 1)
  makeSubRef(payload.capabilities, ...)          ← sub-range of residual_params (Tier 1)
```

### 5.3 STRUCTURE token handling

**PUSH** (entering a container):

- Increment `depth_`, set `is_dict_[depth_]` and `expecting_key_[depth_]`.
- At depth 2, when `current_key_ == "params"`:
  - `in_params_=true`, `params_byte_start_=tok_start`
- At depth 3, when `in_params_` and `!in_sub_container_` and
  `params_key_ == "arguments"` or `"capabilities"`:
  - `in_sub_container_=true`
  - `sub_is_arguments_ = (params_key_ == "arguments")`
  - `sub_container_start_=tok_start`
  - If `sub_is_arguments_`: `arguments_kind_ = to_dict ? JsonObject : JsonArray`

**POP** (closing a container):

- At depth 3, when `in_sub_container_`:
  - `in_sub_container_=false`
  - If `captureEnabled()`:
    - `sub_is_arguments_` → record `arguments_byte_start_/end_`
    - else → record `capabilities_byte_start_/end_`
- At depth 2, when `in_params_`:
  - `in_params_=false`, `params_byte_end_=body_src_pos_`
- Restore `expecting_key_[depth_]` for the parent dict.

The `params_byte_end_` and both sub-container end values are set to
`body_src_pos_` after it has already been advanced past the closing delimiter —
so the ranges are inclusive of the closing `}` or `]` character.

### 5.4 `str_target_` gating rules

```
if string_is_key_:
    str_target_ = &str_acc_           ← always accumulate keys for routing decisions

else if depth_ == 1:
    current_key_ == "id"     → &id_
    current_key_ == "method" → &method_
    anything else            → nullptr  (e.g. "jsonrpc" version string: discarded)

else if depth_ == 2 && in_params_:
    params_key_ == "name"    → &params_name_
    params_key_ == "uri"     → &params_uri_
    params_key_ == "ref"     → &params_ref_
    anything else            → nullptr  (any other params value: discarded)

else (depth_ >= 3, or depth_ == 2 outside params_):
    nullptr                            ← discarded unconditionally
```

For a 4 MB value at depth 3 (e.g. a large base64 blob inside `arguments`):
`str_target_=nullptr`, `appendStringToken` is not called, and no heap allocation
occurs. The bytes exist only in the HTTP chunk buffer that the caller already owns.

### 5.5 Scalar extraction

**NUMBER at depth 1, `current_key_ == "id"`**: the raw token bytes are parsed as
integer or float and stored as a string:

```cpp
if (absl::SimpleAtoi(num, &i_val)) id_ = std::to_string(i_val);
else if (absl::SimpleAtod(num, &d_val)) id_ = std::to_string(static_cast<int64_t>(d_val));
```

This handles both integer and float JSON-RPC ids while normalizing to a string
representation. There are no other numeric fields at depth 1 that `AgentBodyParser`
extracts.

**LITERAL tokens**: no literals are extracted for agent bodies. The LITERAL case
advances `expecting_key_` for the parent dict and breaks.

### 5.6 Body-size tiering

```
┌────────────────────────────────────────────────────────────────────────────┐
│ body size                   │ tier │ behavior                              │
├────────────────────────────────────────────────────────────────────────────┤
│ ≤ max_element_capture_bytes │  1   │ Full capture. arguments and           │
│   (default 256 KB)          │      │ capabilities byte ranges are          │
│                             │      │ recorded. makeSubRef creates          │
│                             │      │ PayloadRef sub-refs for               │
│                             │      │ payload.params_raw,                   │
│                             │      │ payload.arguments, and                │
│                             │      │ payload.capabilities.                 │
├────────────────────────────────────────────────────────────────────────────┤
│ > max_element_capture_bytes │  2   │ Semantic-only. arguments and          │
│ ≤ max_body_bytes (4 MB)     │      │ capabilities byte ranges NOT          │
│                             │      │ recorded. payload.params_raw is       │
│                             │      │ still populated as a zero-copy        │
│                             │      │ sub-range of residual_params.         │
│                             │      │ Routing fields (params_name_,         │
│                             │      │ params_uri_, params_ref_) still       │
│                             │      │ extracted. payload.arguments and      │
│                             │      │ payload.capabilities are empty.       │
├────────────────────────────────────────────────────────────────────────────┤
│ > max_body_bytes            │  3   │ Hard reject. feed() returns           │
│                             │      │ ResourceExhausted immediately.        │
└────────────────────────────────────────────────────────────────────────────┘
```

**Unified code path**: the only Tier 1 vs Tier 2 branch is the `captureEnabled()`
check inside the STRUCTURE POP for depth 3:

```cpp
if (pop_depth == 3 && in_sub_container_) {
    in_sub_container_ = false;
    if (captureEnabled()) {          // ← the only Tier 1 vs Tier 2 branch
        if (sub_is_arguments_) {
            arguments_byte_start_ = sub_container_start_;
            arguments_byte_end_   = body_src_pos_;
        } else {
            capabilities_byte_start_ = sub_container_start_;
            capabilities_byte_end_   = body_src_pos_;
        }
    }
}
```

When `captureEnabled()` returns `false`, the byte-range variables for `arguments`
and `capabilities` are never written. In `finish()`, `makeSubRef` sees `end <= start`
and is a no-op for those fields. `params_byte_start_/end_` is recorded and
`params_raw` is always populated in both tiers.

### 5.7 E2E trace — MCP `tools/call` (Tier 1)

**Request body:**

```json
{
  "jsonrpc": "2.0",
  "id": "req-1",
  "method": "tools/call",
  "params": {
    "name": "read_file",
    "arguments": {"path": "/etc/config.json"}
  }
}
```

Body size: approximately 120 bytes. Well under 256 KB → Tier 1. `captureEnabled()`
returns `true` throughout.

**Token-by-token trace** (`body_src_pos_` starts at 0, `chunk_base = 0`):

```
STRUCTURE PUSH to_dict=true  ← opening '{' of root object
  depth_=1, is_dict_[1]=true, expecting_key_[1]=true

STRING chain "jsonrpc" (key)
  string_is_key_=true → str_target_=&str_acc_; str_acc_="jsonrpc"
  !cont → depth_==1: seen_jsonrpc_=true; current_key_="jsonrpc"; expecting_key_[1]=false

STRING chain "2.0" (value, depth 1, current_key_=="jsonrpc")
  string_is_key_=false, depth_==1, current_key_!="id"/"method"
    → str_target_=nullptr   ← "2.0" discarded, 0 heap
  chain complete: is_dict_[1] → expecting_key_[1]=true

STRING chain "id" (key)
  current_key_="id", seen_id_=true

STRING chain "req-1" (value, depth 1, current_key_=="id")
  str_target_=&id_ → id_="req-1"

STRING chain "method" (key)
  current_key_="method", seen_method_=true

STRING chain "tools/call" (value, depth 1, current_key_=="method")
  str_target_=&method_ → method_="tools/call"

STRING chain "params" (key)
  current_key_="params", seen_params_=true

STRUCTURE PUSH to_dict=true  ← opening '{' of params
  depth_=2, is_dict_[2]=true, expecting_key_[2]=true
  current_key_=="params":
    in_params_=true
    params_byte_start_=tok_start   ← absolute byte offset of '{' in body

STRING chain "name" (key at depth 2, in_params_)
  string_is_key_=true → str_acc_="name"
  !cont → depth_==2 && in_params_: params_key_="name"; expecting_key_[2]=false

STRING chain "read_file" (value, depth 2, in_params_, params_key_=="name")
  string_is_key_=false, depth_==2, in_params_=true, params_key_=="name"
    → params_name_.clear(); str_target_=&params_name_
  str_target_=&params_name_ → params_name_="read_file"
  chain complete: is_dict_[2] → expecting_key_[2]=true

STRING chain "arguments" (key at depth 2, in_params_)
  params_key_="arguments"; expecting_key_[2]=false

STRUCTURE PUSH to_dict=true  ← opening '{' of arguments
  depth_=3, is_dict_[3]=true, expecting_key_[3]=true
  in_params_ && !in_sub_container_ && params_key_=="arguments":
    in_sub_container_=true
    sub_is_arguments_=true
    sub_container_start_=tok_start   ← byte offset of opening '{' of arguments
    arguments_kind_=JsonObject

STRING chain "path" (key at depth 3)
  string_is_key_=true → str_target_=&str_acc_; str_acc_="path"
  chain complete: depth_==3 (not 1 or 2&&in_params_): expecting_key_[3]=false
  (no current_key_ or params_key_ update at depth 3)

STRING chain "/etc/config.json" (value at depth 3)
  string_is_key_=false, depth_==3
    → str_target_=nullptr   ← depth 3 value: 0 heap allocation
  chain complete: is_dict_[3] → expecting_key_[3]=true

STRUCTURE POP  ← closing '}' of arguments
  pop_depth=3, depth_=2
  in_sub_container_=true:
    in_sub_container_=false
    captureEnabled()=true (Tier 1):
      arguments_byte_start_=sub_container_start_
      arguments_byte_end_=body_src_pos_         ← one past closing '}'
  is_dict_[2] → expecting_key_[2]=true

STRUCTURE POP  ← closing '}' of params
  pop_depth=2, depth_=1
  in_params_=true:
    in_params_=false
    params_byte_end_=body_src_pos_              ← one past closing '}'
  is_dict_[1] → expecting_key_[1]=true

STRUCTURE POP  ← closing '}' of root object
  pop_depth=1, depth_=0

decode_tokens returns status==nullptr (OK)
  wuffs_done_=true, break outer
```

**finish():**

```
feedChunk("", closed=true)
  wuffs_done_=true → return OK immediately

request.jsonrpc_id = "req-1"
request.rpc_method = "tools/call"

classify({POST, /mcp, headers, "tools/call"})
  → protocol=AgenticMcp, invocation=AgentInvocation::ToolsCall
  → payload.dialect=AgentDialect::Mcp

populatePayload(payload):
  case ToolsCall: payload.tool_name = params_name_ = "read_file"

payload.residual_params = residual_writer_->finalize()
  → PayloadRef::External{offset=0, len=120}  (full body in mmap arena, no copy)

params_byte_end_ > params_byte_start_:  ✓
  makeSubRef(payload.params_raw, params_byte_start_, plen, residual, JsonObject)
  → PayloadRef::External{mmap_offset + params_byte_start_, plen}  (no copy)

arguments_byte_end_ > arguments_byte_start_:  ✓ (Tier 1)
  makeSubRef(payload.arguments, arguments_byte_start_, alen, residual, JsonObject)
  → PayloadRef::External{mmap_offset + arguments_byte_start_, alen}  (no copy)

capabilities: end==start → makeSubRef is a no-op
```

**Final `AgentPayload` state:**

```
payload.invocation      = AgentInvocation::ToolsCall
payload.dialect         = AgentDialect::Mcp
payload.tool_name       = "read_file"                         (plain std::string)
payload.arguments       = PayloadRef::External{off_A, len_A}  (zero-copy mmap sub-range)
payload.params_raw      = PayloadRef::External{off_P, len_P}  (zero-copy mmap sub-range)
payload.residual_params = PayloadRef::External{0, 120}        (full body in mmap arena)

request.jsonrpc_id  = "req-1"
request.rpc_method  = "tools/call"
```

No intermediate `params_buf_`, no `StringStreamWriter`, no `nlohmann` DOM. All
three sub-refs point into the same mmap region as `residual_params`, created with
pointer arithmetic only.

---

## 6. Memory analysis

### 6.1 Old vs new: the `token_buf_` vulnerability

Both parsers' predecessors used `IncrementalJsonTokenizer`, which maintained a
single `std::string token_buf_` that accumulated every JSON key at every depth
before firing any callback.

| Property | Old `IncrementalJsonTokenizer` | New Wuffs-based (both parsers) |
|---|---|---|
| Key accumulation depth | All depths via `token_buf_` | Keys accumulated into `str_acc_` (bounded by max_body_bytes); depth 3+ keys still go into `str_acc_`, but are bounded by the same ceiling |
| Value accumulation at depth 3+ | Values accumulated into `token_buf_` in non-capture mode; streaming callbacks reduced this but relied on bespoke flag logic | `str_target_=nullptr` unconditionally — never allocates, regardless of value length |
| Parse state across HTTP chunks | 14 C++ enum states, manually preserved | Wuffs stackless coroutine, automatically resumed |
| Token size bound | Unbounded (whole string per token event) | 65535 bytes per Wuffs token (16-bit `tlen`) |
| Correctness guarantee | Bespoke state machine, no formal proof | Wuffs toolchain verifies memory safety |

### 6.2 Depth 3+ value attack

```
InferenceBodyParser attack body:
{"model":"gpt-4o","messages":[{"role":"user","content":"<4 MB base64>"}]}

Old IncrementalJsonTokenizer (Tier 2):
  At content value at depth 3:
    token_buf_ grows proportionally to the 4 MB string
    even with streaming callbacks: bespoke str_target_=null check required
  → heap growth O(4 MB) before the guard fires

New Wuffs-based InferenceBodyParser:
  ~62 STRING COPY tokens arrive for the 4 MB content value
  For each token: depth_==3 → str_target_=nullptr → appendStringToken not called
  → 0 bytes heap allocated for all 62 tokens
  Guarantee: structural, not flag-dependent
```

```
AgentBodyParser attack body:
{"method":"tools/call","params":{"name":"foo","arguments":{"data":"<4 MB base64>"}}}

Old IncrementalJsonTokenizer (Tier 2):
  At "data" value at depth 3 (inside arguments):
    token_buf_ grows to 4 MB
  → heap growth O(4 MB)

New Wuffs-based AgentBodyParser:
  depth_==3 → str_target_=nullptr → 0 bytes heap allocated
```

### 6.3 Depth-2 key accumulation bound

An attacker controlling a **key** at depth 2 (inside `params` for agent bodies, or
inside `messages[i]` elements for inference) receives `str_target_ = &str_acc_`
because all keys are accumulated for routing. However:

1. Wuffs tokens are bounded at 65535 bytes each. Per-token heap growth is capped.
2. Total body size is bounded by `max_body_bytes` (enforced in `feed()` before any
   parsing runs). The attacker cannot force heap growth beyond this ceiling.
3. For agent bodies: a depth-2 key in `params` that does not match `"name"`,
   `"uri"`, `"ref"`, `"arguments"`, or `"capabilities"` causes `params_key_` to
   hold that string, and the next value immediately gets `str_target_=nullptr`.
4. For inference bodies: depth-2 keys (inside `messages[i]` elements) go into
   `str_acc_`, which is cleared at the start of the next key string chain.

### 6.4 Peak heap by tier — both parsers

**InferenceBodyParser:**

| Component | Tier 1 (≤ 256 KB) | Tier 2 (≤ 4 MB) | Tier 3 (reject) |
|---|---|---|---|
| `dec_` (Wuffs decoder) | ~2 KB | ~2 KB | n/a |
| `tok_data_[256]` in-object | 2048 B | 2048 B | n/a |
| `residual_writer_` metadata | ~48 B | ~48 B | n/a |
| `model_`, `str_acc_` | O(field length) | O(field length) | n/a |
| `sampling_` (stop strings) | O(Σ stop lengths) | O(Σ stop lengths) | n/a |
| Depth 3+ values | **0** (str_target_=nullptr) | **0** (str_target_=nullptr) | n/a |
| **Total peak heap** | **~3 KB + O(keys)** | **~3 KB + O(keys)** | **≤ one chunk** |
| **Peak RSS (mmap)** | **Body size (evictable)** | **Body size (evictable)** | **0** |

**AgentBodyParser:**

| Component | Tier 1 (≤ 256 KB) | Tier 2 (≤ 4 MB) | Tier 3 (reject) |
|---|---|---|---|
| `dec_` (Wuffs decoder) | ~2 KB | ~2 KB | n/a |
| `tok_data_[256]` in-object | 2048 B | 2048 B | n/a |
| `residual_writer_` metadata | ~48 B | ~48 B | n/a |
| `id_`, `method_`, routing fields | O(field length) | O(field length) | n/a |
| `str_acc_` (max) | O(longest key at depth 1–2) | O(longest key at depth 1–2) | n/a |
| Depth 3+ anything | **0** (str_target_=nullptr) | **0** (str_target_=nullptr) | n/a |
| `PayloadRef::External` handles | 12 bytes each | 12 bytes each | n/a |
| **Total peak heap** | **~3 KB + O(keys)** | **~3 KB + O(keys)** | **≤ one chunk** |
| **Peak RSS (mmap)** | **Body size (evictable)** | **Body size (evictable)** | **0** |

There is no meaningful heap cost difference between Tier 1 and Tier 2 in either
parser. Both tiers run the same code and have the same peak heap. The only
difference is which `PayloadRef`s are populated in `finish()`.

---

## 7. Invariants and security guarantees

The following six invariants hold for both `InferenceBodyParser` and
`AgentBodyParser` identically. They are consequences of the shared `feedChunk`
skeleton and `str_target_` discipline.

### Invariant 1 — `body_src_pos_` is always exact

`body_src_pos_` advances by `tlen` for every token, including FILLER and DROP
tokens. The byte positions recorded for element ranges, params ranges, and
sub-container ranges are always true offsets of those characters in the raw body
stream that `residual_writer_` has captured. `makeSubRef` can safely index into
`residual_params` using these offsets without any additional bookkeeping.

### Invariant 2 — Sub-refs are always valid subsets of residual

`makeSubRef` creates a sub-ref only when `end > start` and `residual` is
non-empty. Start positions are set on STRUCTURE PUSH (opening delimiter); end
positions are set on STRUCTURE POP (`body_src_pos_` already advanced through the
closing character). Because `residual_writer_->append(chunk)` runs before
`feedChunk(chunk, ...)`, the full body including both delimiters is in
`residual_params` by the time `finish()` is called.

### Invariant 3 — `wuffs_done_` prevents double-processing

Once `wuffs_done_` is set (on OK status, end-of-data note, or any path that sets
it), all subsequent `feedChunk` calls return `OkStatus` immediately. This prevents
re-entry into the Wuffs coroutine after the document is complete. `finish()` calls
`feedChunk("", true)` to flush any trailing number token (numbers have no
terminator character); for a document that already returned OK this is a no-op.

### Invariant 4 — Duplicate-key detection is inline and early

`InferenceBodyParser` guards: `seen_model_`, `seen_stream_`, `seen_messages_`,
`seen_tools_`, `seen_temperature_`, `seen_top_p_`, `seen_max_tokens_`, `seen_n_`,
`seen_seed_`, `seen_stop_`.

`AgentBodyParser` guards: `seen_jsonrpc_`, `seen_id_`, `seen_method_`,
`seen_params_`.

In both parsers, the check fires when a STRING token chain completes as a depth-1
key. On the second occurrence, `has_error_` is set and the inner token loop breaks
immediately. The outer loop returns `InvalidArgumentError` from `feedChunk`, which
propagates through `feed()` to `RequestDecoder::onData()`, which sends a 400
response before any auth or upstream routing runs. Detection fires at parse time —
as soon as the duplicate key's closing quote is processed — not at `finish()` time.

### Invariant 5 — `str_target_` null-safety

`appendStringToken` is called only when `str_target_ != nullptr && tlen > 0`. The
null check is unconditional and precedes the call. `str_target_` is reset to
`nullptr` at string completion (the `if (!cont)` block). It cannot be left pointing
at a freed string object because the pointed-to strings are data members that
outlive all `feedChunk` calls.

### Invariant 6 — `in_chain_` spans `feed()` boundaries safely

If a STRING token chain spans an HTTP chunk boundary (Wuffs returns `short_read`
mid-chain), the next `feed()` call enters `feedChunk` with `in_chain_=true`. The
`first_in_group = !in_chain_` check correctly identifies the next token as a
continuation. `str_acc_` is not cleared and `str_target_` is not re-selected.
String accumulation continues from where it stopped.

### Security guarantee — bounded heap for attacker-controlled content

For any attacker-controlled body up to `max_body_bytes` (default 4 MB):

**InferenceBodyParser:**
- Depth 1 values (`model_`, stop strings): bounded by `max_body_bytes`.
- Depth 2 values (inside `messages[]`/`tools[]`): `str_target_=nullptr`. Zero heap.
- Depth 3+ values (inside elements): `str_target_=nullptr`. **Zero heap.** This is
  the property the old tokenizer violated for depth-3+ content.
- All keys at any depth: accumulate into `str_acc_`, bounded by `max_body_bytes`.

**AgentBodyParser:**
- Depth 1 values (`id_`, `method_`): bounded by `max_body_bytes`.
- Depth 2 values in `params` (known fields): `params_name_`, `params_uri_`,
  `params_ref_` — short in practice; bounded by `max_body_bytes`.
- Depth 2 values in `params` (unknown fields): `str_target_=nullptr`. Zero heap.
- Depth 3+ anything: `str_target_=nullptr`. **Zero heap.**
- All keys at any depth: accumulate into `str_acc_`, bounded by `max_body_bytes`.

Peak attacker-controlled heap allocation is bounded by `max_body_bytes`, enforced
before any parsing. The Wuffs token model adds a secondary bound: no single
operation allocates more than 65535 bytes per token.

---

## 8. Build

The Wuffs JSON decoder is vendored as an amalgamated single file:

```
source/extensions/filters/http/ai_protocol_manager/codec/wuffs-v0.4.c
```

It is compiled as a single-file library (the standard Wuffs distribution method).
`WUFFS_IMPLEMENTATION` must be defined in exactly one compilation unit:

| File | Role |
|---|---|
| A dedicated `.c` compilation unit | Defines `WUFFS_IMPLEMENTATION`, compiled as C to avoid C++-only warnings in the generated code |
| Other consumers | Include `wuffs-v0.4.c` without `WUFFS_IMPLEMENTATION` (declarations only) |

Both `InferenceBodyParser` and `AgentBodyParser` are implemented in
`request_decoder.cc`, which includes the Wuffs header via the
`ai_protocol_manager` build target. Wuffs requires no runtime dependencies beyond
the C standard library. `wuffs_json__decoder::alloc()` is the only dynamic
allocation Wuffs makes; all other operations are purely computational.

---

## 9. Related documentation

- **PARSING.md** — full description of `PayloadStore`, `MmapPayloadStore`,
  `MmapStreamWriter`, async prefetch (`prefetchExternalPayloadRefs`), the
  body-size tiering system, `PayloadRef` storage variants, and the configuration
  thresholds (`max_body_bytes`, `max_element_capture_bytes`, `max_inline_bytes`).
  Components shared by both parsers — `residual_writer_`, `PayloadRef`,
  `makeSubRef`, and the dispatch pipeline — are documented in full there.

- `request_decoder.cc` lines 27–63 — `appendStringToken` free function (anonymous
  namespace, shared by both parsers).

- `request_decoder.cc` lines 81–400 — complete `InferenceBodyParser`
  implementation.

- `request_decoder.cc` lines 416–769 — complete `AgentBodyParser` implementation.

- `request_decoder.h` — `DecoderConfig` field definitions (`max_body_bytes`,
  `max_element_capture_bytes`, `max_inline_bytes`).
