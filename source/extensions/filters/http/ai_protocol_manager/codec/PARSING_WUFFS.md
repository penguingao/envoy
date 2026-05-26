# AI Protocol Parsing

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

## 2. End-to-end pipeline overview

```
╔══════════════════════════════════════════════════════════════════════════════════════════════════╗
║                          Envoy AI Protocol Manager — Request Pipeline                           ║
╚══════════════════════════════════════════════════════════════════════════════════════════════════╝

  Downstream client                                                        Upstream provider
       │                                                                          │
       │  POST /mcp                              POST /v1/chat/completions        │
       │  Content-Type: application/json         Content-Type: application/json   │
       ▼                                                   ▼
┌─────────────────────────────────────────────────────────────────────────────────────────────────┐
│  decodeHeaders()                                                                                │
│                                                                                                 │
│   RequestDecoder::onHeaders()                                                                   │
│     ProtocolClassifier::classify(method, path, headers)                                         │
│              │                                       │                                          │
│         AgenticMcp                              Inference                                       │
│              │                                       │                                          │
│     agent_parser_ = new                   inference_parser_ = new                              │
│       AgentBodyParser(config, store)         InferenceBodyParser(config, store)                 │
│              │                                       │                                          │
│         state = ParsingAgentBody              state = ParsingInferenceBody                      │
└─────────────────────────────────────────────────────────────────────────────────────────────────┘
       │                                                   │
       │  data chunk(s)                        data chunk(s)
       ▼                                                   ▼
┌───────────────────────────────┐           ┌─────────────────────────────────┐
│  decodeData() × N             │           │  decodeData() × N               │
│                               │           │                                 │
│  AgentBodyParser::feed(chunk) │           │  InferenceBodyParser::feed(chunk)│
│    total_bytes_ += chunk.size │           │    total_bytes_ += chunk.size   │
│    if > max_body_bytes → 413  │           │    if > max_body_bytes → 413    │
│                               │           │                                 │
│    residual_writer_           │           │    residual_writer_             │
│    ->append(chunk)            │           │    ->append(chunk)              │
│    ┌────────────────────┐     │           │    ┌────────────────────┐       │
│    │ MmapStreamWriter   │     │           │    │ MmapStreamWriter   │       │
│    │ memcpy → mmap arena│     │           │    │ memcpy → mmap arena│       │
│    └────────────────────┘     │           │    └────────────────────┘       │
│                               │           │                                 │
│    feedChunk(chunk, false)    │           │    feedChunk(chunk, false)      │
│    ┌────────────────────────┐ │           │    ┌──────────────────────────┐ │
│    │ Wuffs token loop       │ │           │    │ Wuffs token loop         │ │
│    │                        │ │           │    │                          │ │
│    │ depth 1:               │ │           │    │ depth 1:                 │ │
│    │  "method" → method_    │ │           │    │  "model"  → model_       │ │
│    │  "id"     → id_        │ │           │    │  "stream" → streaming_   │ │
│    │                        │ │           │    │  numbers  → sampling_    │ │
│    │ depth 2 (in_params_):  │ │           │    │  "stop"   → sampling_    │ │
│    │  "name" → params_name_ │ │           │    │                          │ │
│    │  "uri"  → params_uri_  │ │           │    │ depth 2:                 │ │
│    │                        │ │           │    │  "messages"→in_messages_ │ │
│    │ depth 3 (arguments/    │ │           │    │  "tools"  → in_tools_    │ │
│    │  capabilities):        │ │           │    │                          │ │
│    │  range start recorded  │ │           │    │ depth 3 (in_messages_ or │ │
│    │  str_target_=nullptr   │ │           │    │  in_tools_):             │ │
│    │  → 0 bytes heap        │ │           │    │  elem_start_ recorded    │ │
│    │                        │ │           │    │  str_target_=nullptr     │ │
│    │ dup key? → 400 inline  │ │           │    │  → 0 bytes heap          │ │
│    └────────────────────────┘ │           │    │                          │ │
│                               │           │    │ dup key? → 400 inline    │ │
│    short_read → wait for next │           │    └──────────────────────────┘ │
│    chunk (Wuffs coroutine     │           │                                 │
│    preserves all parse state) │           │    short_read → wait for next   │
└───────────────────────────────┘           │    chunk                        │
                                            └─────────────────────────────────┘
       │  end_stream                                    │  end_stream
       ▼                                               ▼
┌───────────────────────────────┐           ┌─────────────────────────────────┐
│  AgentBodyParser::finish()    │           │  InferenceBodyParser::finish()  │
│                               │           │                                 │
│  feedChunk("", closed=true)   │           │  feedChunk("", closed=true)     │
│  (flushes trailing number     │           │  (flushes trailing number token)│
│   token; no-op if done)       │           │                                 │
│                               │           │  residual_writer_->finalize()   │
│  residual_writer_->finalize() │           │  → PayloadRef::External         │
│  → PayloadRef::External       │           │    {offset=0, len=body_size}    │
│    {offset=0, len=body_size}  │           │  (full body, zero-copy)         │
│  (full body, zero-copy)       │           │                                 │
│                               │           │  for each message_ranges_[i]:   │
│  makeSubRef(params_raw,       │           │    makeSubRef(messages[i])      │
│    params_start, params_len)  │           │    → External{base+start, len}  │
│  → External{base+start, len}  │           │    (pointer arithmetic only)    │
│  (pointer arithmetic only)    │           │                                 │
│                               │           │  for each tool_ranges_[i]:      │
│  if captureEnabled():         │           │    makeSubRef(tools[i])         │
│    makeSubRef(arguments,      │           │    → External{base+start, len}  │
│      arg_start, arg_len)      │           │                                 │
│    → External{base+start,len} │           │  AiRequest {                    │
│    makeSubRef(capabilities,…) │           │    InferencePayload {           │
│                               │           │      target.name = model_       │
│  AiRequest {                  │           │      sampling    = sampling_    │
│    rpc_method = method_       │           │      streaming   = streaming_   │
│    jsonrpc_id = id_           │           │      messages[]  = [External…]  │
│    AgentPayload {             │           │      tools[]     = [External…]  │
│      tool_name   = name_      │           │      residual    = External      │
│      arguments   = External   │           │    }                            │
│      params_raw  = External   │           │  }                              │
│      residual    = External   │           │                                 │
│    }                          │           │  state = BodyComplete           │
│  }                            │           └─────────────────────────────────┘
│  state = BodyComplete         │
└───────────────────────────────┘
       │                                               │
       └────────────────────┬──────────────────────────┘
                            │
                            ▼
           ┌─────────────────────────────────────────┐
           │  prefetchExternalPayloadRefs()           │
           │                                          │
           │  collect all External PayloadRefs        │
           │  atomic<int> pending = refs.size()       │
           │                                          │
           │  for each External ref:                  │
           │    store.fetchAsync(ref, dispatcher, cb) │
           │    ┌───────────────────────────────────┐ │
           │    │ detached thread                   │ │
           │    │   pread(fd, buf, len, offset)     │ │
           │    │   ← may page-fault off event loop │ │
           │    │   dispatcher.post(cb(buf))        │ │
           │    └───────────────────────────────────┘ │
           │                                          │
           │  cb(): ref ← Buffered; --pending         │
           │  pending==0 → on_done()                  │
           │  (all refs now Inline or Buffered;        │
           │   toString() safe, no mmap access)        │
           └─────────────────────────────────────────┘
                            │
                            ▼
       ┌───────────────────────────────────────────────────┐
       │  Filter sub-chain (auth, rate-limit, routing…)    │
       │                                                   │
       │  MCP path:              Inference path:           │
       │  read AgentPayload      read InferencePayload     │
       │  tool_name → policy     target.name → provider   │
       │  params_raw → audit log sampling → rate limit     │
       │  arguments → transform  messages → content check  │
       └───────────────────────────────────────────────────┘
                            │
                            ▼
       ┌───────────────────────────────────────────────────┐
       │  RequestEncoder                                   │
       │                                                   │
       │  MCP path:              Inference path:           │
       │  AgentRequestEncoder    InferenceRequestEncoder   │
       │                                                   │
       │  rebuild JSON-RPC       rebuild REST JSON         │
       │  envelope from          body from                 │
       │  AiRequest fields:      AiRequest fields:         │
       │    id, method,            model, stream,          │
       │    params_raw             sampling, messages,     │
       │    (Buffered → str)       tools                   │
       │                          (Buffered → str)         │
       │  set upstream headers   set upstream headers      │
       │  Authorization: Bearer… Authorization: Bearer…    │
       └───────────────────────────────────────────────────┘
                            │
                            ▼
                     upstream provider
                  (MCP server / OpenAI API)
```

**Key properties visible in the diagram:**

| Property | Where it appears |
|---|---|
| Wuffs stackless coroutine | `feedChunk` preserves parse state across `decodeData` calls; `short_read` exits cleanly |
| Zero heap for depth-3+ content | `str_target_=nullptr` in both token loops regardless of value size |
| All body bytes land in mmap once | `decodeData` feeds each `Buffer::RawSlice` directly to `onData` — no intermediate `toString()` allocation; `residual_writer_->append(chunk)` is the only copy, from the network buffer into the mmap arena |
| Sub-refs are pointer arithmetic | `makeSubRef` → `External{base+start, len}`, no data copied |
| Async page-fault isolation | `pread` thread; event loop never blocks on read page-fault |
| Encoders see only Buffered/Inline | `prefetchExternalPayloadRefs` completes before filter sub-chain runs |
| Duplicate-key rejection is inline | `has_error_` set during the Wuffs token loop; 400 returned before `finish()` |

---

## 3. Wuffs token model

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

## 4. Shared infrastructure

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

## 5. InferenceBodyParser

`InferenceBodyParser` is a private inner class of `RequestDecoder`, defined in
`request_decoder.cc` (lines 81–400). It is constructed when request headers
classify the incoming request as an OpenAI inference body
(`POST /v1/chat/completions`, `/v1/completions`, `/v1/embeddings`, etc.).

### 5.1 Class overview

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

### 5.2 Lifecycle

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

### 5.3 STRUCTURE token handling

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

### 5.4 `str_target_` gating rules

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

### 5.5 Scalar extraction

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

### 5.6 Body-size tiering

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

### 5.7 E2E trace — chat/completions (Tier 1)

**Request body** (≈90 bytes — Tier 1, `captureEnabled()` = true throughout):

```json
{"model":"gpt-4o","stream":true,"max_tokens":512,"messages":[{"role":"user","content":"Hi"}]}
```

**Token-by-token trace** (`body_src_pos_` starts at 0, `chunk_base = 0`).
FILLER tokens (whitespace, colons, commas) advance `body_src_pos_` and are omitted.

```
Token                                  d      str_target_       State change
───────────────────────────────────────────────────────────────────────────────────────────
PUSH {  root object                    0→1    —                 is_dict_[1]=T
                                                                expecting_key_[1]=T
───────────────────────────────────────────────────────────────────────────────────────────
KEY  "model"                           1      &str_acc_         current_key_="model"
                                                                seen_model_=T
                                                                expecting_key_[1]=F
VAL  "gpt-4o"       key==model         1      &model_           model_="gpt-4o"
                                                                expecting_key_[1]=T
───────────────────────────────────────────────────────────────────────────────────────────
KEY  "stream"                          1      &str_acc_         current_key_="stream"
                                                                seen_stream_=T
                                                                expecting_key_[1]=F
LIT  true           key==stream        1      —                 streaming_=true
                                                                expecting_key_[1]=T
───────────────────────────────────────────────────────────────────────────────────────────
KEY  "max_tokens"                      1      &str_acc_         current_key_="max_tokens"
                                                                seen_max_tokens_=T
                                                                expecting_key_[1]=F
NUM  512            key==max_tokens    1      —                 SimpleAtoi →
                                                                  sampling_.max_tokens=512
                                                                expecting_key_[1]=T
───────────────────────────────────────────────────────────────────────────────────────────
KEY  "messages"                        1      &str_acc_         current_key_="messages"
                                                                seen_messages_=T
                                                                expecting_key_[1]=F
PUSH [  messages array                 1→2    —                 is_dict_[2]=F
                                                                in_messages_=T
PUSH {  messages[0]                    2→3    —                 is_dict_[3]=T
                                                                in_elem_=T
                                                                elem_start_=offset('{')
───────────────────────────────────────────────────────────────────────────────────────────
KEY  "role"         depth 3            3      &str_acc_         str_acc_="role"
                                                                (no current_key_ update at d=3)
VAL  "user"         depth 3            3      nullptr  ◀━━━━━  0 bytes heap
KEY  "content"      depth 3            3      &str_acc_         str_acc_="content"
VAL  "Hi"           depth 3            3      nullptr  ◀━━━━━  0 bytes heap
───────────────────────────────────────────────────────────────────────────────────────────
POP }  messages[0]                     3→2    —                 in_elem_=F
                                                                message_ranges_ ←
                                                                  {elem_start_, body_src_pos_}
                                                                message_kinds_ ← JsonObject
POP ]  messages array                  2→1    —                 in_messages_=F
                                                                expecting_key_[1]=T
POP }  root object                     1→0    —                 —
───────────────────────────────────────────────────────────────────────────────────────────
STATUS OK                              —      —                 wuffs_done_=T  break outer
```

**finish():**

```
feedChunk("", closed=true)
  wuffs_done_=T → return OK immediately (no re-entry into Wuffs coroutine)

payload.target.name         = "gpt-4o"
payload.sampling.max_tokens = 512
request.streaming           = true

residual_params = residual_writer_->finalize()
               = External{offset=0, len=90}            ← full body, no copy

message_ranges_[0] = {elem_start, elem_end}
makeSubRef → messages[0] = External{base+elem_start, elem_len}   pointer arithmetic only

tool_ranges_ empty → payload.tools = []
```

**Final `InferencePayload`:**

| Field | Value | Note |
|---|---|---|
| `target.name` | `"gpt-4o"` | plain std::string |
| `sampling.max_tokens` | `512` | int32_t |
| `streaming` | `true` | bool |
| `messages[0]` | `External{off_M, len_M}` | zero-copy mmap sub-range |
| `tools` | `[]` | empty — no tools key in body |
| `residual_params` | `External{0, 90}` | full body in mmap arena |
---

## 6. AgentBodyParser

`AgentBodyParser` is a private inner class of `RequestDecoder`, defined in
`request_decoder.cc` (lines 416–769). It is constructed when request headers
classify an incoming request as an agent (MCP or A2A) JSON-RPC body.

### 6.1 Class overview

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

### 6.2 Lifecycle

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

### 6.3 STRUCTURE token handling

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

### 6.4 `str_target_` gating rules

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

### 6.5 Scalar extraction

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

### 6.6 Body-size tiering

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

### 6.7 E2E trace — MCP `tools/call` (Tier 1)

**Request body** (≈120 bytes — Tier 1, `captureEnabled()` = true throughout):

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

**Token-by-token trace** (`body_src_pos_` starts at 0, `chunk_base = 0`).
FILLER tokens (whitespace, colons, commas) advance `body_src_pos_` and are omitted.

```
Token                                    d      str_target_       State change
─────────────────────────────────────────────────────────────────────────────────────────────
PUSH {  root object                      0→1    —                 is_dict_[1]=T
                                                                  expecting_key_[1]=T
─────────────────────────────────────────────────────────────────────────────────────────────
KEY  "jsonrpc"                           1      &str_acc_         current_key_="jsonrpc"
                                                                  seen_jsonrpc_=T
                                                                  expecting_key_[1]=F
VAL  "2.0"      key≠"id", key≠"method"  1      nullptr  ◀━━━━━  0 bytes heap
                                                                  expecting_key_[1]=T
─────────────────────────────────────────────────────────────────────────────────────────────
KEY  "id"                                1      &str_acc_         current_key_="id"
                                                                  seen_id_=T
                                                                  expecting_key_[1]=F
VAL  "req-1"    key==id                  1      &id_              id_="req-1"
                                                                  expecting_key_[1]=T
─────────────────────────────────────────────────────────────────────────────────────────────
KEY  "method"                            1      &str_acc_         current_key_="method"
                                                                  seen_method_=T
                                                                  expecting_key_[1]=F
VAL  "tools/call"  key==method           1      &method_          method_="tools/call"
                                                                  expecting_key_[1]=T
─────────────────────────────────────────────────────────────────────────────────────────────
KEY  "params"                            1      &str_acc_         current_key_="params"
                                                                  seen_params_=T
                                                                  expecting_key_[1]=F
PUSH {  params object                    1→2    —                 is_dict_[2]=T
                                                                  in_params_=T
                                                                  params_byte_start_=offset('{')
─────────────────────────────────────────────────────────────────────────────────────────────
KEY  "name"     depth 2, in_params_      2      &str_acc_         params_key_="name"
                                                                  expecting_key_[2]=F
VAL  "read_file"  params_key==name       2      &params_name_     params_name_="read_file"
                                                                  expecting_key_[2]=T
─────────────────────────────────────────────────────────────────────────────────────────────
KEY  "arguments"  depth 2, in_params_    2      &str_acc_         params_key_="arguments"
                                                                  expecting_key_[2]=F
PUSH {  arguments object                 2→3    —                 is_dict_[3]=T
                                                                  in_sub_container_=T
                                                                  sub_is_arguments_=T
                                                                  sub_container_start_=offset('{')
                                                                  arguments_kind_=JsonObject
─────────────────────────────────────────────────────────────────────────────────────────────
KEY  "path"     depth 3                  3      &str_acc_         str_acc_="path"
                                                                  (no params_key_ update at d=3)
VAL  "/etc/config.json"  depth 3         3      nullptr  ◀━━━━━  0 bytes heap
─────────────────────────────────────────────────────────────────────────────────────────────
POP }  arguments object                  3→2    —                 in_sub_container_=F
                                                                  captureEnabled()=T → record:
                                                                    arguments_byte_start_=
                                                                      sub_container_start_
                                                                    arguments_byte_end_=
                                                                      body_src_pos_
                                                                  expecting_key_[2]=T
POP }  params object                     2→1    —                 in_params_=F
                                                                  params_byte_end_=body_src_pos_
                                                                  expecting_key_[1]=T
POP }  root object                       1→0    —                 —
─────────────────────────────────────────────────────────────────────────────────────────────
STATUS OK                                —      —                 wuffs_done_=T  break outer
```

**finish():**

```
feedChunk("", closed=true)
  wuffs_done_=T → return OK immediately (no re-entry into Wuffs coroutine)

request.jsonrpc_id = "req-1"
request.rpc_method = "tools/call"

classify(POST, /mcp, "tools/call")
  → protocol=AgenticMcp  invocation=ToolsCall  dialect=Mcp

populatePayload: payload.tool_name = params_name_ = "read_file"

residual_params = residual_writer_->finalize()
               = External{offset=0, len=120}           ← full body, no copy

params_byte_end_ > params_byte_start_:  ✓
  makeSubRef → params_raw  = External{base+params_start, params_len}   pointer arithmetic
arguments_byte_end_ > arguments_byte_start_:  ✓  (Tier 1)
  makeSubRef → arguments   = External{base+arg_start, arg_len}          pointer arithmetic
capabilities: end==start → makeSubRef is a no-op
```

**Final `AgentPayload`:**

| Field | Value | Note |
|---|---|---|
| `invocation` | `AgentInvocation::ToolsCall` | classified from method |
| `dialect` | `AgentDialect::Mcp` | classified from path |
| `tool_name` | `"read_file"` | plain std::string |
| `arguments` | `External{off_A, len_A}` | zero-copy mmap sub-range |
| `params_raw` | `External{off_P, len_P}` | zero-copy mmap sub-range |
| `residual_params` | `External{0, 120}` | full body in mmap arena |

```
request.jsonrpc_id = "req-1"
request.rpc_method = "tools/call"
```

All three `External` refs point into the same mmap region as `residual_params`.
No intermediate copy, no `nlohmann` DOM, no `StringStreamWriter`.

---

## 7. Memory analysis

### 7.1 Old vs new: the `token_buf_` vulnerability

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

### 7.2 Depth 3+ value attack

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

### 7.3 Depth-2 key accumulation bound

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

### 7.4 Peak heap by tier — both parsers

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

## 8. Invariants and security guarantees

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

## 9. Configuration

`DecoderConfig` is defined in `request_decoder.h` and is the single
configuration knob for both parsers:

```cpp
struct DecoderConfig {
  size_t max_inline_bytes{4096};
  size_t max_body_bytes{4 * 1024 * 1024};
  size_t max_element_capture_bytes{256 * 1024};
};
```

| Field | Default | Role |
|---|---|---|
| `max_inline_bytes` | 4 KB | Fields at or below this size are stored as `PayloadRef::Inline` (inside the ref itself). Larger fields become `Buffered` (InMemoryPayloadStore) or `External` (MmapPayloadStore). |
| `max_body_bytes` | 4 MB | Hard limit. `feed()` returns `ResourceExhausted` the moment accumulated bytes exceed this ceiling, before any JSON parsing runs for that chunk. Attacker-controlled heap growth is bounded by this value regardless of body content. |
| `max_element_capture_bytes` | 256 KB | Soft limit controlling Tier 1 vs Tier 2. Bodies at or below this size have per-element byte ranges recorded — `messages[]`/`tools[]` for inference, `arguments`/`capabilities` for agent. Bodies above this limit still extract all scalar fields (Tier 2) but individual element `PayloadRef`s are not populated. |

Both parsers hold a `const DecoderConfig&` reference (not a copy). The config is
owned by the outer filter and outlives the decoder.

---

## 10. PayloadRef storage model

`PayloadRef` (defined in `ai_payload.h`) is a lightweight discriminated-union
handle to a field value. All large field values in `InferencePayload` and
`AgentPayload` are typed as `PayloadRef` rather than `std::string` or
`Buffer::Instance` to avoid copying large content out of the store.

### 10.1 Storage variants

```
PayloadRef::Storage
├── Inline    — value lives in std::string inline_data_ inside the ref (≤ max_inline_bytes)
├── Buffered  — value lives in Buffer::OwnedImpl on the heap (> max_inline_bytes, mmap unavailable)
└── External  — value lives in MmapPayloadStore's mmap region; ref holds {uint64_t offset, size_t length}
```

| Variant | Contents | `toString()` | `size()` | Typical origin |
|---|---|---|---|---|
| `Inline` | `std::string inline_data_` | Direct return | `inline_data_.size()` | Small fields ≤ `max_inline_bytes` |
| `Buffered` | `Buffer::InstancePtr buffered_data_` | `buffered_data_->toString()` | `buffered_data_->length()` | Heap fallback when mmap unavailable |
| `External` | `uint64_t external_offset_`, `size_t external_length_` | **PANIC** | `external_length_` | MmapPayloadStore normal path |

**Critical**: calling `toString()` on an `External` ref panics at runtime:

```cpp
case Storage::External:
  PANIC("External PayloadRef must be materialized through PayloadStore::fetch()");
```

Encoders must call `convertPayloadRefToString(ref, request)` instead, which
routes through `request.payload_store->fetch()`. The panic surfaces at test time
any encoder that fails to handle the External variant.

### 10.2 Sub-refs and `makeSubRef`

Both parsers create sub-refs of `residual_params` for nested fields
(`messages[]`, `tools[]`, `arguments`, `capabilities`, `params_raw`).
`makeSubRef` in `request_decoder.cc` produces the correct variant depending on
the storage of the parent:

```
parent is External:
  PayloadRef::makeExternal(parent.externalOffset() + field_start, field_length)
  → shares the same mmap region; zero additional copy

parent is Inline:
  store_.store(parent.inlineView().substr(field_start, field_length), kind)
  → always Inline (sub-range is also small)

parent is Buffered:
  store_.store(extracted bytes, kind)
  → Inline or Buffered depending on field_length vs max_inline_bytes
```

`makeSubRef` is a no-op when `field_length == 0` or the parent ref is empty.

### 10.3 PayloadRef storage variant decision tree

```
body arrives ──► residual_writer_->append(chunk)   (each HTTP chunk)
                      │
                      │ finalize()
                      ▼
             total_written_ ≤ max_inline_bytes?
                  ├── Yes → PayloadRef::Inline   (copy back from mmap region)
                  └── No  → mmap available (fd_ != -1)?
                                ├── Yes → PayloadRef::External{start_offset, total_written_}
                                └── No  → PayloadRef::Buffered (heap fallback)

Each sub-ref (messages[i], tools[i], arguments, params_raw):
    parent External? → makeExternal(parent.offset + field_start, len)  [zero-copy]
    parent Inline?   → store_.store(sub-string, kind)                   [small copy]
    parent Buffered? → store_.store(extracted bytes, kind)              [heap copy]
```

---

## 11. StreamWriter and PayloadStore interfaces

Both parsers call `store_.beginStore(PayloadKind::JsonObject)` on the first
`feed()` call to open a streaming write session, then call
`residual_writer_->append(chunk)` for every subsequent chunk. This captures the
raw body bytes into the store without any intermediate copy beyond what the
backend requires.

### 11.1 `StreamWriter` interface

```cpp
class StreamWriter {
public:
  virtual ~StreamWriter() = default;
  virtual void append(absl::string_view bytes) = 0;
  virtual PayloadRef finalize() = 0;
};
```

`append` is called once per HTTP data chunk. `finalize` is called exactly once
in `finish()` after all chunks have been fed. The returned `PayloadRef` is stored
as `payload.residual_params` and represents the full request body.

### 11.2 `PayloadStore` interface

```cpp
class PayloadStore {
public:
  virtual PayloadRef store(std::string data, PayloadKind kind) = 0;
  virtual PayloadRef store(Buffer::Instance& data, PayloadKind kind) = 0;
  virtual std::unique_ptr<StreamWriter> beginStore(PayloadKind kind) = 0;
  virtual void fetch(const PayloadRef& ref, FetchCallback cb) = 0;
  virtual void fetchAsync(const PayloadRef& ref,
                          Event::Dispatcher& dispatcher,
                          FetchCallback cb);
};
```

Two implementations are provided:

| Implementation | File | Large fields | `fetchAsync` |
|---|---|---|---|
| `InMemoryPayloadStore` | `ai_payload.h/.cc` | `Buffer::OwnedImpl` (Buffered) | Synchronous (calls `fetch` inline) |
| `MmapPayloadStore` | `mmap_payload_store.h/.cc` | Mmap arena (External) | Async `pread` on detached thread |

`AiRequest::payload_store` holds a non-owning pointer to the store. The outer
filter creates and owns the store; the decoder and encoders access it through
this pointer.

---

## 12. MmapPayloadStore

`MmapPayloadStore` (in `mmap_payload_store.h/.cc`) offloads large payloads to
an anonymous temp file via `mmap`, keeping only fields at or below
`max_inline_bytes` in process heap memory.

### 12.1 Backing file

```cpp
std::string tmpl = absl::StrCat(tmp_dir, "/envoy_payload_XXXXXX");
fd_ = ::mkstemp(tmpl.data());
::unlink(tmpl.c_str());
ensureSpace(kInitialCapacity);
```

The file is created with `mkstemp` and immediately unlinked. It has no
directory entry after `unlink` but remains accessible via `fd_` until the
store is destroyed. `~MmapPayloadStore` calls `munmap` then `close(fd_)`. When
the fd is closed the OS reclaims all pages — including on abnormal process exit,
since the file has no path.

### 12.2 Bump-allocated arena layout

The mmap region is a flat byte array. Each write advances `write_offset_`:

```
mmap region:
  [  residual_body_1  |  residual_body_2  | ... |  unused  ]
   ↑                   ↑
   off_1               off_2
   ←── len_1 ────────→←── len_2 ──→              ↑
   0                                         write_offset_
                                                           ↑
                                                       capacity_
```

`PayloadRef::External` stores `{offset, length}` into this flat region. All
sub-refs point into the same region as their parent.

### 12.3 Capacity growth

Initial capacity: `kInitialCapacity = 64 KB`. On overflow:

1. `ftruncate(fd_, new_cap)` extends the backing file.
2. Linux: `mremap(MREMAP_MAYMOVE)` extends in place or relocates.
3. macOS: `munmap` + `mmap` at the new size (no `mremap` available).
4. Doubles `new_cap` until `write_offset_ + needed ≤ new_cap`.

### 12.4 Fallback

If `mkstemp` fails, `fd_ = -1` and every `store()` call falls back to
`PayloadRef::Buffered` (`Buffer::OwnedImpl`). `MmapStreamWriter` sets
`failed_=true` on any subsequent `ensureSpace` failure and `finalize()` produces
a `Buffered` ref in that case. The store is always functional; failure degrades
from External to Buffered, not to an error.

### 12.5 Zero-copy `Buffer::Instance` ingestion

The `store(Buffer::Instance& data, ...)` overload walks the slab chain directly:

```cpp
const Buffer::RawSliceVector slices = data.getRawSlices();
for (const auto& s : slices) {
    std::memcpy(map_ + write_offset_, s.mem_, s.len_);
    write_offset_ += s.len_;
}
```

No intermediate contiguous copy is required. Each slab is `memcpy`'d once,
sequentially, into the mmap region.

### 12.6 Thread safety

`MmapPayloadStore` is not thread-safe. One store per request stream is the
intended usage pattern, matching Envoy's single-threaded filter chain.
`fetchAsync` spawns a detached thread to perform the `pread`, but that thread
captures `fd_` by value (an `int`) and uses only the POSIX `pread` system call —
it never touches the store object.

---

## 13. MmapStreamWriter

`MmapStreamWriter` is a nested class inside `MmapPayloadStore`. It opens a
streaming session at the current `write_offset_` of its parent store.

```cpp
MmapStreamWriter(MmapPayloadStore& store, PayloadKind /*kind*/)
    : store_(store), start_offset_(store.write_offset_) {}

void append(absl::string_view bytes) {
    if (failed_ || bytes.empty()) return;
    if (store_.fd_ == -1 || !store_.ensureSpace(bytes.size())) {
        failed_ = true; return;
    }
    store_.appendBytes(bytes.data(), bytes.size());
    total_written_ += bytes.size();
}

PayloadRef finalize() {
    if (failed_ || store_.fd_ == -1) {
        // Heap fallback: copy whatever was written from the mmap region.
        auto buf = std::make_unique<Buffer::OwnedImpl>();
        if (!failed_ && total_written_ > 0)
            buf->add(store_.map_ + start_offset_, total_written_);
        return PayloadRef::makeBuffered(std::move(buf));
    }
    if (total_written_ <= store_.max_inline_bytes_) {
        // Small session: copy back from mmap to inline.
        // The mmap space is left allocated — waste ≤ max_inline_bytes_.
        return PayloadRef::makeInline(std::string(
            reinterpret_cast<char*>(store_.map_ + start_offset_), total_written_));
    }
    return PayloadRef::makeExternal(
        static_cast<uint64_t>(start_offset_), total_written_);
}
```

Key properties:

- `start_offset_` is captured at construction and is the byte offset of the
  first byte this writer owns. All bytes in
  `[start_offset_, start_offset_ + total_written_)` belong to this writer.
- Multiple `StreamWriter` sessions in the same store are sequential — their byte
  ranges are non-overlapping and contiguous.
- `finalize()` returns External for large sessions, Inline for small ones.
  Small sessions write to the mmap region and then copy back (one copy, bounded
  by `max_inline_bytes_`). The arena space for small sessions is not reclaimed.

---

## 14. External payload fetch pipeline

`External` refs cannot be materialized by `PayloadRef::toString()`. Three layers
handle materialization, from fine-grained to coarse-grained:

### Layer 1 — `PayloadStore::fetch` (synchronous, single ref)

```cpp
void MmapPayloadStore::fetch(const PayloadRef& ref, FetchCallback cb) {
    if (ref.storage() == PayloadRef::Storage::External) {
        auto buf = std::make_unique<Buffer::OwnedImpl>();
        buf->add(map_ + ref.externalOffset(), ref.externalLength());
        cb(std::move(buf));
        return;
    }
    cb(std::make_unique<Buffer::OwnedImpl>(ref.toString()));
}
```

Reads from the mmap region on the calling thread. If the page is not in the
page cache, this causes a read page fault on the event loop thread. Used only in
tests and for Inline/Buffered refs (which have no page-fault risk).

### Layer 2 — `MmapPayloadStore::fetchAsync` (async pread, single ref)

```cpp
void MmapPayloadStore::fetchAsync(const PayloadRef& ref,
                                  Event::Dispatcher& dispatcher,
                                  FetchCallback cb) {
    if (ref.storage() != PayloadRef::Storage::External || fd_ == -1) {
        fetch(ref, std::move(cb));  // synchronous fallback for non-External
        return;
    }
    const int fd = fd_;                          // captured by value
    const uint64_t off = ref.externalOffset();
    const size_t   len = ref.externalLength();
    std::thread([fd, off, len, &dispatcher, cb = std::move(cb)]() mutable {
        std::vector<char> tmp(len);
        auto buf = std::make_unique<Buffer::OwnedImpl>();
        if (len > 0 && ::pread(fd, tmp.data(), len, (off_t)off) == (ssize_t)len)
            buf->add(tmp.data(), len);
        dispatcher.post([buf = std::move(buf), cb = std::move(cb)]() mutable {
            cb(std::move(buf));
        });
    }).detach();
}
```

- The detached thread captures `fd` by value. If the store is destroyed and `fd`
  is closed before `pread` runs, `pread` returns -1 and the callback posts an
  empty buffer — no crash.
- The `pread` (which may page-fault) runs off the event loop. The callback is
  posted back to the dispatcher thread when the read completes.

### Layer 3 — `prefetchExternalPayloadRefs` (fan-out, all refs in a request)

```cpp
void prefetchExternalPayloadRefs(AiRequest& request,
                                 Event::Dispatcher& dispatcher,
                                 std::function<void()> on_done);
```

Called in the dispatch pipeline after `RequestDecoder::onEndStream()` succeeds
and before any encoder runs. The sequence:

1. Collect all `External` `PayloadRef`s from the request's payload into a flat list.
2. Create an `std::atomic<int>` countdown initialized to the list size.
3. For each `External` ref, call `payload_store->fetchAsync(ref, dispatcher, cb)`.
4. Each callback: upgrade the ref in place (External → Buffered), decrement the counter.
5. When the counter reaches zero: call `on_done()` on the dispatcher thread.
6. If there are no External refs: call `on_done()` immediately, no threads spawned.

After `on_done()` fires, every `PayloadRef` in the request is Inline or Buffered.
Encoders can call `ref.toString()` and `convertPayloadRefToString(ref, request)`
without any mmap access.

### Why writes are synchronous

HTTP body chunks arrive sequentially on the event-loop thread.
`MmapStreamWriter::append()` calls `memcpy` into the mmap region — this triggers
**write page faults**, not read page faults. On Linux, write page faults for
MAP_SHARED pages backed by a temp file are handled by the kernel's page allocator
in microseconds and do not block the event loop for meaningful durations. Read
page faults — when the OS evicts a page and a subsequent access faults it back —
are the expensive case. Those are offloaded to the `pread` thread by `fetchAsync`.
Write page faults on freshly-allocated pages are effectively zero additional cost
relative to the `memcpy` itself.

---

## 15. Build

The Wuffs JSON decoder is vendored as an amalgamated single file:

```
source/extensions/filters/http/ai_protocol_manager/codec/wuffs-v0.4.c
```

It is compiled as a single-file library (the standard Wuffs distribution method).
`WUFFS_IMPLEMENTATION` must be defined in exactly one compilation unit:

| File | Role |
|---|---|
| `wuffs_impl.c` | Defines `WUFFS_IMPLEMENTATION`, compiled as C to avoid C++-only warnings in the generated code |
| Other consumers | Include `wuffs-v0.4.c` without `WUFFS_IMPLEMENTATION` (declarations only) |

Both `InferenceBodyParser` and `AgentBodyParser` are implemented in
`request_decoder.cc`, which includes the Wuffs header via the
`ai_protocol_manager` build target. Wuffs requires no runtime dependencies beyond
the C standard library. `wuffs_json__decoder::alloc()` is the only dynamic
allocation Wuffs makes; all other operations are purely computational.

---

## 16. Parser library comparison

### 16.1 The three requirements that determine the choice

This use case is unusual. Most JSON parsing is "parse this complete document and
give me a tree." The proxy needs something different:

1. **True resumable streaming** — HTTP body arrives in N chunks of arbitrary size;
   the parser must resume across chunk boundaries without blocking the event loop.
2. **Pre-accumulation discard** — routing fields are extracted; everything else
   (4 MB `content` values, `arguments` blobs) must be discardable *before* any
   heap allocation occurs for that content. SAX-style libraries (RapidJSON, YAJL)
   accumulate the full string into an internal buffer before firing a callback;
   by the time application code runs, the 4 MB allocation has already happened.
   The discard decision must be possible at the *first byte* of a token, not after
   the last.
3. **Raw byte positions in the source** — given the zero-copy `PayloadRef::External`
   design, `makeSubRef` needs exact byte offsets of `params`, `arguments`, and
   `messages[i]` inside the original body to produce sub-ranges without copying.
   (If fields were copied into owned storage instead, this requirement would not
   exist and YAJL would be a viable streaming option.)

Almost every mainstream library fails at least one of these.

### 16.2 Library-by-library analysis

#### nlohmann/json (prior incumbent for `finish()`)

| Property | Result |
|---|---|
| Streaming | None — requires complete document |
| Heap per large value | O(document size) — full DOM tree |
| Raw byte positions | No |
| Formal safety | No |

A DOM parser. A 4 MB body allocates proportional heap for every node and every
string. It was used for the second-pass parse in the original `AgentBodyParser::finish()`.
No path to streaming, no ability to discard content cheaply.

#### RapidJSON (SAX mode)

RapidJSON has a SAX `Reader` that fires callbacks (`StartObject`, `Key`, `String`,
`EndObject`). It looks promising but has three gaps:

**Gap 1 — Not a resumable coroutine.** `Reader` reads from an `IStream` until
exhausted; there is no way to feed a partial chunk and resume on the next one
without blocking. Envoy's filter chain requires returning from `decodeData()`
immediately — blocking inside a callback to wait for more data is not possible.

**Gap 2 — String callbacks receive decoded strings, not raw bytes.** When the SAX
fires `String(const char* str, SizeType len, bool copy)`, the string has already
been fully decoded into an internal buffer. For a 4 MB `content` value, RapidJSON
allocates 4 MB before calling the handler. The `str_target_=nullptr` discard
optimization is impossible — you cannot instruct RapidJSON to skip a string before
it has already accumulated it.

**Gap 3 — No raw byte positions.** SAX callbacks hand decoded text with no source
offset. `makeSubRef` would be impossible.

#### simdjson

The fastest JSON parser available (~3 GB/s on modern hardware via SIMD). Has both
a DOM API and an "On Demand" lazy API that skips fields never accessed.

**Fatal constraint: requires the full document in a contiguous buffer.** This is
architectural. simdjson's SIMD approach processes 64 bytes at a time across the
whole document and requires padding bytes after the last byte. HTTP chunks arrive
un-padded and non-contiguous. There is no streaming mode. The On Demand API still
requires the complete body in memory before any traversal begins.

#### YAJL (Yet Another JSON Library)

A C streaming callback parser. Genuinely streaming — partial input can be fed
incrementally. The closest alternative to Wuffs for the streaming requirement.

**Gap: string callbacks receive fully accumulated strings.** YAJL's
`yajl_callbacks` fires `yajl_string(ctx, val, len)` with a `const unsigned char*`
that YAJL has fully decoded and buffered internally. For a 4 MB string, YAJL
allocates 4 MB before the callback fires. This is structurally identical to the
`token_buf_` vulnerability in the old `IncrementalJsonTokenizer` — just in a C
library instead of a bespoke state machine.

**Gap: no byte positions.** Callbacks receive decoded text, not source offsets.

#### jsmn

Minimal C tokenizer. Returns an array of `jsmntok_t` with `.start`/`.end` byte
offsets into the source buffer. Does not copy or decode strings — you index back
into the source yourself.

Interesting properties: raw byte positions, no string allocation. But it requires
the **full document** before tokenizing. Not a streaming parser. No chunk-by-chunk
feeding.

#### Custom IncrementalJsonTokenizer (predecessor)

A bespoke 14-state machine. Truly streaming but had the vulnerability described in
§1: `token_buf_` accumulated every JSON key at every nesting depth before firing
any callback. No per-token size bound, no formal proof, ongoing maintenance burden.

### 16.3 Why Wuffs satisfies all three requirements uniquely

| Requirement | Wuffs mechanism |
|---|---|
| Resumable streaming | Stackless coroutine in `dec_` (~2 KB fixed struct). `decode_tokens` returns `short_read` suspension; resumes from the exact source byte on the next `feed()` call. No C++ call-stack state to preserve. |
| Per-token discard | `str_target_=nullptr` means `appendStringToken` is never called. A 4 MB value produces ~62 tokens of ≤65535 bytes each; all 62 are discarded in O(1) with zero heap. The discard is structural, not flag-dependent. |
| Raw byte positions | Every token carries `tlen`. `body_src_pos_` advances by `tlen` for every token including FILLER and DROP tokens. This gives exact byte offsets for `makeSubRef` → `PayloadRef::External{base + start, len}` without any intermediate copy. |
| Formal safety | The Wuffs toolchain proves memory safety at compile time. The 65535-byte `tlen` ceiling is a property of the 16-bit field type, not a runtime check. |

The formal verification is the tie-breaker over a hypothetical YAJL variant with a
"peek-and-discard" callback. A proxy handling attacker-controlled traffic benefits
from a verifiable safety guarantee that no bespoke or community C++ library provides.

### 16.4 Wuffs costs

| Cost | Impact |
|---|---|
| Manual escape decoding | `appendStringToken` was written by hand. The `\uXXXX` surrogate pair limitation (documented in `request_decoder.cc`) is a direct consequence. |
| Continued-token handling | Multi-token strings (>65535 bytes or containing escape sequences) require `in_chain_` state across `feed()` boundaries — ~6 lines of state and logic that no SAX library requires. |
| VBC/VBD flag reading | Token category/detail bitfield decoding is lower-level than SAX callbacks. The `switch(vbc)` block is harder to read at a glance than `OnString(...)`. |
| Community | Wuffs is a single-author research project. RapidJSON and simdjson have orders of magnitude more users and issue history. |
| Build complexity | The `WUFFS_IMPLEMENTATION` single-TU constraint requires `wuffs_impl.c` as a separate compilation unit. Non-obvious to newcomers. |

### 16.5 Verdict

For a security-sensitive streaming proxy, Wuffs is the only off-the-shelf library
that satisfies all three constraints simultaneously. The cost is API complexity —
manual escape decoding and continued-token state — which is why `appendStringToken`
and `in_chain_` require careful documentation.

If the zero-copy sub-ref requirement were dropped (i.e. field bytes were copied
into owned strings), YAJL with a depth-gated discard patch would be a viable
alternative. With the sub-ref requirement in place, no other mainstream library
provides resumable streaming, per-token discardability, and raw byte positions
together.

---

## 17. Related documentation

- `request_decoder.cc` lines 27–63 — `appendStringToken` free function
  (anonymous namespace, shared by both parsers).

- `request_decoder.cc` lines 81–400 — complete `InferenceBodyParser`
  implementation.

- `request_decoder.cc` lines 416–769 — complete `AgentBodyParser` implementation.

- `request_decoder.h` — `DecoderConfig` field definitions and `RequestDecoder`
  class declaration.

- `ai_payload.h` / `ai_payload.cc` — `PayloadRef`, `StreamWriter`,
  `PayloadStore`, `InMemoryPayloadStore`, `convertPayloadRefToString`,
  `prefetchExternalPayloadRefs`.

- `mmap_payload_store.h` / `mmap_payload_store.cc` — `MmapPayloadStore`,
  `MmapStreamWriter`.
