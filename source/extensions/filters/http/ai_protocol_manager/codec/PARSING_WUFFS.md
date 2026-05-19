# Wuffs Streaming Agent Body Parser

## Motivation

### The old `IncrementalJsonTokenizer` vulnerability

The original `AgentBodyParser` (now replaced) used `IncrementalJsonTokenizer` — a
bespoke 14-state machine — to parse JSON-RPC request bodies. In Tier 2 semantic
mode (body too large for `params` capture), the tokenizer still had to walk the
entire body to extract routing fields. The state machine's key accumulation path
used a single `std::string token_buf_` that grew with every JSON key it
encountered, at any nesting depth.

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
choosing a long key name inside a nested object. The tokenizer accumulated every
key at every depth without distinction between depth-1 keys (which must be
accumulated for routing) and depth-2+ keys (which are irrelevant and should be
discarded).

A secondary issue was structural complexity. The 14-state machine, multiplied with
a depth counter and several boolean flags (`in_params_`, `in_sub_container_`, etc.),
produced a bespoke nesting-aware parser with no formal correctness guarantee.

Additionally, the Tier 1 path ran `nlohmann::json::parse()` on the captured
`params_buf_` at `finish()` time, building a full DOM tree in O(params_size) heap
just to extract three routing fields before discarding the tree.

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

## Architecture

### Class overview

`AgentBodyParser` is a private inner class of `RequestDecoder`, defined in
`request_decoder.cc` (lines 784–1189). It is constructed when the request headers
classify an incoming request as an agent (MCP or A2A) JSON-RPC body.

```
RequestDecoder
  └─ AgentBodyParser
       ├─ wuffs_json__decoder::unique_ptr  dec_              ← stackless coroutine (~2 KB)
       ├─ wuffs_base__token  tok_data_[256]                  ← 2048-byte token ring (in-object)
       ├─ wuffs_base__token_buffer  tok_buf_                 ← slice wrapper over tok_data_
       ├─ size_t  body_src_pos_                              ← monotonic body byte counter
       ├─ bool  wuffs_done_                                  ← EOF/complete sentinel
       ├─ std::unique_ptr<StreamWriter>  residual_writer_    ← full-body streaming capture
       ├─ int  depth_                                        ← current nesting depth
       ├─ bool  is_dict_[8], expecting_key_[8]              ← per-depth structure state
       ├─ bool  in_chain_, string_is_key_                   ← current string tracking
       ├─ std::string  str_acc_                             ← key/value accumulator
       ├─ std::string*  str_target_                         ← where to write current string
       ├─ std::string  current_key_, id_, method_           ← depth-1 extracted fields
       ├─ bool  in_params_                                  ← inside params container
       ├─ size_t  params_byte_start_, params_byte_end_      ← byte range of params in body
       ├─ std::string  params_key_, params_name_,           ← params routing fields
       │               params_uri_, params_ref_
       ├─ bool  in_sub_container_, sub_is_arguments_        ← inside arguments/capabilities
       ├─ size_t  sub_container_start_                      ← opening byte of sub-container
       ├─ size_t  arguments_byte_start_/end_                ← byte range of arguments
       ├─ size_t  capabilities_byte_start_/end_             ← byte range of capabilities
       ├─ bool  seen_jsonrpc_, seen_id_, seen_method_,      ← duplicate-key guards
       │         seen_params_
       └─ bool  has_error_; std::string  error_             ← inline error state
```

### Key invariant: `body_src_pos_` is always exact

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

The byte positions recorded for `params_byte_start_`, `arguments_byte_end_`, etc.
are always true offsets into the raw body stream that `residual_writer_` has
captured. `makeSubRef` can safely index into `residual_params` using these offsets.

### Lifecycle

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
  request.rpc_method = method_
  request.jsonrpc_id = id_
  classify(http_method_, path_, headers_, rpc_method) → payload.invocation / dialect
  populatePayload(payload)                       ← route params_name_/uri_/ref_
  payload.residual_params = residual_writer_->finalize()
  makeSubRef(payload.params_raw, ...)            ← sub-range of residual_params
  makeSubRef(payload.arguments, ...)             ← sub-range of residual_params (Tier 1)
  makeSubRef(payload.capabilities, ...)          ← sub-range of residual_params (Tier 1)
```

---

## Wuffs token model

Every call to `wuffs_json__decoder__decode_tokens()` fills `tok_buf_` with as
many tokens as fit in the 256-slot ring, then returns a status. Tokens are consumed
in the inner loop before the next decode call. Four token classes matter for
`AgentBodyParser`:

| VBC constant | Meaning | Action |
|---|---|---|
| `FILLER` | Whitespace, commas, colons | Advance `body_src_pos_` by `tlen`; no other action |
| `STRUCTURE` | Object/array open or close | Manage `depth_`, `is_dict_[]`, `expecting_key_[]`; record byte ranges |
| `STRING` | String content, quotes, or escapes | Gate on `str_target_`; call `appendStringToken` when non-null |
| `NUMBER` or `LITERAL` | Numeric or `true`/`false`/`null` | Extract `id_` if applicable; advance `expecting_key_` |

### The `continued` flag and multi-token strings

A single JSON string may span multiple Wuffs tokens when it is longer than 65535
bytes or contains escape sequences that force a token boundary. The `continued`
flag (`cont`) is `true` on all tokens except the last of a chain. `AgentBodyParser`
tracks the chain state with `in_chain_`:

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

## `feedChunk` loop — complete control flow

```
feedChunk(chunk, closed):
  if wuffs_done_: return OK

  chunk_base = body_src_pos_                     ← absolute body offset of chunk[0]
  src_buf = wuffs_base__ptr_u8__reader(chunk, closed)

  outer loop (until break):
    status = decode_tokens(dec_, &tok_buf_, &src_buf, empty_slice)

    inner loop — drain tok_buf_:
      tok  = tok_buf_.data.ptr[tok_buf_.meta.ri++]
      vbc  = token_value_base_category(tok)
      vbd  = token_value_base_detail(tok)
      tlen = token_length(tok)                   ← max 65535 bytes
      cont = token_continued(tok)

      tok_start      = body_src_pos_
      body_src_pos_ += tlen                      ← advance for EVERY token

      switch vbc:

        FILLER:
          (nothing — body_src_pos_ already advanced)

        STRUCTURE PUSH (is_push && to_dict):
          ++depth_
          is_dict_[depth_] = to_dict
          expecting_key_[depth_] = to_dict

          if depth_==2 && current_key_=="params":
            in_params_ = true
            params_byte_start_ = tok_start       ← offset of opening '{' or '['

          if depth_==3 && in_params_ && !in_sub_container_ &&
             (params_key_=="arguments" || params_key_=="capabilities"):
            in_sub_container_ = true
            sub_is_arguments_ = (params_key_=="arguments")
            sub_container_start_ = tok_start     ← offset of opening '{' or '['
            if sub_is_arguments_: arguments_kind_ = to_dict ? JsonObject : JsonArray

        STRUCTURE POP:
          pop_depth = depth_
          --depth_

          if pop_depth==3 && in_sub_container_:
            in_sub_container_ = false
            if captureEnabled():                 ← only in Tier 1
              if sub_is_arguments_:
                arguments_byte_start_ = sub_container_start_
                arguments_byte_end_   = body_src_pos_   ← one past closing '}' or ']'
              else:
                capabilities_byte_start_ = sub_container_start_
                capabilities_byte_end_   = body_src_pos_

          if pop_depth==2 && in_params_:
            in_params_ = false
            params_byte_end_ = body_src_pos_     ← one past closing '}' or ']'

          if depth_>=1 && is_dict_[depth_]:
            expecting_key_[depth_] = true        ← parent dict awaits next key

        STRING:
          raw = chunk.substr(tok_start - chunk_base, tlen)
          first_in_group = !in_chain_

          if first_in_group:
            str_acc_.clear()
            string_is_key_ = is_dict_[depth_] && expecting_key_[depth_]
            str_target_ = pick_target()          ← see "str_target_ gating" below

          if str_target_ && tlen > 0:
            appendStringToken(*str_target_, raw, vbd)

          in_chain_ = cont

          if !cont (string chain complete):
            if string_is_key_:
              if depth_==1: duplicate check; current_key_ = str_acc_; expecting_key_[1]=false
              if depth_==2 && in_params_: params_key_ = str_acc_; expecting_key_[2]=false
            else:
              if is_dict_[depth_]: expecting_key_[depth_] = true
            str_target_ = nullptr

        NUMBER:
          if depth_==1 && current_key_=="id":
            num = chunk.substr(tok_start - chunk_base, tlen)
            SimpleAtoi/Atod → id_ = std::to_string(parsed_value)
          if is_dict_[depth_]: expecting_key_[depth_] = true

        LITERAL:
          if is_dict_[depth_]: expecting_key_[depth_] = true

      if has_error_: break inner loop

    if has_error_: return InvalidArgumentError

    inspect status:
      nullptr      → wuffs_done_=true, break outer
      is_note      → wuffs_done_=true, break outer
      short_read   → break outer (return OK — wait for next feed())
      short_write  → tok_buf_.meta.ri=wi=0, continue outer
      error        → return InvalidArgumentError
```

---

## String accumulation — `str_target_` gating

The central memory-safety mechanism is `str_target_`: a pointer to the `std::string`
that should receive the decoded bytes of the current string token chain, or `nullptr`
to discard. It is set once at the first token of each string and held for the entire
chain.

### Target selection (inside `feedChunk`, when `first_in_group`)

```
if string_is_key_:                          ← is_dict_[depth_] && expecting_key_[depth_]
    str_target_ = &str_acc_                 ← always accumulate keys for routing decisions

else if depth_ == 1:
    current_key_ == "id"     → &id_
    current_key_ == "method" → &method_
    anything else            → nullptr      ← e.g. "jsonrpc" version string: discarded

else if depth_ == 2 && in_params_:
    params_key_ == "name"    → &params_name_
    params_key_ == "uri"     → &params_uri_
    params_key_ == "ref"     → &params_ref_
    anything else            → nullptr      ← any other value inside params: discarded

else (depth_ >= 3, or depth_ == 2 outside params_):
    nullptr                                 ← discarded unconditionally
```

### Why depth-3+ values are safe

An attacker controlling a string value at depth 3 or deeper (e.g. any value inside
`arguments`) receives `str_target_ = nullptr`. The `appendStringToken` call is not
made. The raw bytes exist only in the HTTP chunk buffer (a `string_view` into
Envoy's existing data frame); no heap allocation occurs for that content regardless
of its length.

For a 4 MB value at depth 3, the heap cost is exactly zero — the token's bytes are
read in-place from the source buffer that the caller already owns.

### Depth-2 key accumulation

An attacker controlling a **key** at depth 2 (inside `params`) receives
`str_target_ = &str_acc_`. Keys are always accumulated because the parser needs to
compare them against known field names (`"name"`, `"uri"`, `"ref"`,
`"arguments"`, `"capabilities"`) to set routing state. However:

1. Wuffs tokens are bounded at 65535 bytes each. Per-token heap growth is capped.
2. Total key length is bounded by `max_body_bytes` (enforced in `feed()` before any
   parsing runs). The attacker cannot force heap growth beyond this ceiling.
3. A depth-2 key that does not match any known field name sets `params_key_` to that
   long string, then the next value token immediately gets `str_target_ = nullptr`
   (because the unknown key is not in the known-field set). The key string itself
   lives in `params_key_` until overwritten by the next key.

This is the old `token_buf_` problem reduced in scope: it now applies only to
depth-2 keys (not depth-3+ keys or any values at depth 3+), and it is bounded by
the same `max_body_bytes` that limits the entire request.

### Comparison with old design

```
Old IncrementalJsonTokenizer (Tier 2):
  token_buf_ accumulates EVERY key at EVERY depth
  token_buf_ also accumulates string values when not using streaming callbacks
  A 4 MB key at depth 2, 3, or any depth → 4 MB token_buf_ growth

New Wuffs-based:
  str_acc_ accumulates keys at depth 1 and depth 2 only
  depth 3+ keys: str_acc_ grows (bounded by max_body_bytes)
  depth 3+ values: str_target_=nullptr — 0 bytes
  depth 2 values (unknown field): str_target_=nullptr — 0 bytes
  depth 1 values (unknown field): str_target_=nullptr — 0 bytes
  depth 1 values (id, method): accumulated into id_/method_ (short in practice)
```

---

## Body-size tiering

`DecoderConfig` exposes two thresholds that divide the body-size space into three
tiers. `AgentBodyParser` participates in the same tiering system as
`InferenceBodyParser`; the thresholds and their defaults are identical.

```
┌────────────────────────────────────────────────────────────────────────────┐
│ body size                   │ tier │ behavior                              │
├────────────────────────────────────────────────────────────────────────────┤
│ ≤ max_element_capture_bytes │  1   │ Full capture. params_raw, arguments,  │
│   (default 256 KB)          │      │ and capabilities byte ranges are all  │
│                             │      │ recorded. makeSubRef creates External  │
│                             │      │ refs for all three.                   │
├────────────────────────────────────────────────────────────────────────────┤
│ > max_element_capture_bytes │  2   │ Semantic-only. arguments and          │
│ ≤ max_body_bytes (4 MB)     │      │ capabilities byte ranges are NOT      │
│                             │      │ recorded. params_raw is still created │
│                             │      │ as a zero-copy sub-range of           │
│                             │      │ residual_params. Routing fields       │
│                             │      │ (params_name_, params_uri_,           │
│                             │      │ params_ref_) still extracted.         │
├────────────────────────────────────────────────────────────────────────────┤
│ > max_body_bytes            │  3   │ Hard reject. feed() returns           │
│                             │      │ ResourceExhausted immediately.        │
└────────────────────────────────────────────────────────────────────────────┘
```

### Unified code path — the single Tier 1 / Tier 2 branch

Tier 1 and Tier 2 share exactly the same `feedChunk` code. There is no separate
handler, no mode switch, and no InCapture path for params. The only branch is a
single `captureEnabled()` check inside the STRUCTURE POP case for depth 3:

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

`captureEnabled()` is:

```cpp
bool captureEnabled() const {
    return total_bytes_ <= config_.max_element_capture_bytes;
}
```

When `captureEnabled()` returns `false`, the byte-range variables for arguments and
capabilities are never written. In `finish()`, `makeSubRef` sees `end <= start` and
is a no-op for those fields. All other behavior — `params_byte_start_/end_`,
`params_name_/uri_/ref_`, `method_`, `id_` — is identical in both tiers.

### `makeSubRef` — zero-copy sub-range creation

After `residual_writer_->finalize()` produces a `PayloadRef` for the full body,
sub-refs are created for the byte ranges that were recorded:

```cpp
void makeSubRef(PayloadRef& ref, size_t start, size_t len,
                const PayloadRef& residual, PayloadKind kind) {
    if (len == 0 || residual.empty()) return;
    if (residual.storage() == PayloadRef::Storage::External) {
        ref = PayloadRef::makeExternal(residual.externalOffset() + start, len);
    } else {
        std::string full = residual.toString();
        if (start < full.size()) {
            ref = store_.store(full.substr(start, std::min(len, full.size() - start)), kind);
        }
    }
}
```

For `MmapPayloadStore` (production): `residual` is `External{mmap_offset, body_len}`.
The sub-ref is `External{mmap_offset + start, len}` — a 12-byte struct pointing
into the same already-mapped mmap region. No copy, no new mmap call, no additional
RSS growth.

For `InMemoryPayloadStore` (tests): `residual` is `Buffered`. A `substr()` copy is
made. This is acceptable for the test-only backend.

---

## Full E2E trace — MCP `tools/call` (Tier 1)

### Request body

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

### Phase 1 — Headers

```
decodeHeaders(headers, end_stream=false)
  └─ RequestDecoder::onHeaders(headers)
       ├─ classify(POST, /mcp, application/json)
       │    → heuristic: POST + JSON body → candidate ProtocolKind::AgenticMcp
       │    (rpc_method unknown until body is parsed — second classify pass needed)
       ├─ state_ = ParsingAgentBody
       └─ AgentBodyParser constructed:
            dec_             = wuffs_json__decoder::alloc()   (~2 KB, one-time alloc)
            tok_data_[256]   in-object, no heap
            tok_buf_         wraps tok_data_
            residual_writer_ = nullptr                        (lazy — opened on first chunk)
```

### Phase 2 — Body chunks

Assume the entire body arrives in one HTTP data frame (chunk = full body,
chunk_base = 0).

```
decodeData(chunk, end_stream=false)
  └─ AgentBodyParser::feed(chunk)
       ├─ total_bytes_ += 120                        [120 ≤ max_body_bytes ✓]
       ├─ residual_writer_ = store_.beginStore(JsonObject)
       │    → MmapStreamWriter{ start_offset_=0, total_written_=0 }
       ├─ residual_writer_->append(chunk)            ← 120 bytes to mmap arena (RSS)
       └─ feedChunk(chunk, closed=false)
```

Token-by-token trace. `body_src_pos_` starts at 0, `chunk_base = 0`.

```
Tokens 1–2: STRUCTURE PUSH to_dict=true, FILLER "\n  "
  depth_=1, is_dict_[1]=true, expecting_key_[1]=true
  body_src_pos_=4

Tokens 3–5: STRING DROP '"', COPY "jsonrpc", DROP '"'
  first_in_group=true, string_is_key_=true (is_dict_[1] && expecting_key_[1])
  str_target_=&str_acc_
  DROP tokens: no append. COPY: str_acc_="jsonrpc". Final DROP: chain complete.
  !cont → depth_==1: seen=&seen_jsonrpc_; seen_jsonrpc_=true; current_key_="jsonrpc"
          expecting_key_[1]=false

Token 6: FILLER ":"
Token 7–9: STRING DROP '"', COPY "2.0", DROP '"'
  first_in_group=true, string_is_key_=false, depth_==1, current_key_=="jsonrpc"
    → str_target_=nullptr  (not "id" or "method")
  "2.0" bytes arrive as COPY token; str_target_=null → appendStringToken not called
  chain complete: is_dict_[1] → expecting_key_[1]=true

[Similar pattern for "id":"req-1"]
  At key "id": current_key_="id", seen_id_=true
  At value "req-1": str_target_=&id_ → id_="req-1"
  chain complete: expecting_key_[1]=true

[Similar pattern for "method":"tools/call"]
  At key "method": current_key_="method", seen_method_=true
  At value "tools/call": str_target_=&method_ → method_="tools/call"
  chain complete: expecting_key_[1]=true

Key "params": current_key_="params", seen_params_=true

FILLER ":"

STRUCTURE PUSH to_dict=true  ← opening '{' of params
  depth_=2, is_dict_[2]=true, expecting_key_[2]=true
  depth_==2 && current_key_=="params":
    in_params_=true
    params_byte_start_=tok_start   ← absolute byte offset of '{' in body

Key "name" (depth 2, in_params_):
  string_is_key_=true (is_dict_[2] && expecting_key_[2])
  str_acc_="name"
  chain complete: depth_==2 && in_params_ → params_key_="name"; expecting_key_[2]=false

Value "read_file" (depth 2, in_params_, params_key_=="name"):
  first_in_group: string_is_key_=false, depth_==2, in_params_=true, params_key_=="name"
    → params_name_.clear(); str_target_=&params_name_
  COPY tokens: params_name_="read_file"
  chain complete: is_dict_[2] → expecting_key_[2]=true

Key "arguments" (depth 2, in_params_):
  str_acc_="arguments"
  params_key_="arguments"; expecting_key_[2]=false

STRUCTURE PUSH to_dict=true  ← opening '{' of arguments
  depth_=3, is_dict_[3]=true, expecting_key_[3]=true
  depth_==3 && in_params_ && !in_sub_container_ && params_key_=="arguments":
    in_sub_container_=true
    sub_is_arguments_=true
    sub_container_start_=tok_start   ← byte offset of opening '{' of arguments
    arguments_kind_=JsonObject

Key "path" (depth 3, in_sub_container_):
  string_is_key_=true (is_dict_[3] && expecting_key_[3])
  str_target_=&str_acc_  ← key always accumulated
  str_acc_="path"
  chain complete: depth_==3, not depth_==1 and not (depth_==2 && in_params_)
    → no current_key_ or params_key_ update; expecting_key_[3]=false

Value "/etc/config.json" (depth 3):
  first_in_group: string_is_key_=false, depth_==3
    → str_target_=nullptr  ← depth 3 value: discarded unconditionally
  bytes arrive as COPY token; str_target_=null → 0 heap allocation
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
  wuffs_done_=true
  break outer loop

feedChunk returns OkStatus
```

### Phase 3 — End of stream

```
decodeData(last_chunk, end_stream=true)   [if body arrived in one frame: same as above]

  └─ AgentBodyParser::finish(payload, request)
       ├─ feedChunk("", closed=true)
       │    wuffs_done_=true → return OK immediately
       │
       ├─ request.rpc_method = "tools/call"
       ├─ request.jsonrpc_id = "req-1"
       │
       ├─ classify({POST, /mcp, headers, "tools/call"})
       │    → protocol=AgenticMcp, invocation=AgentInvocation::ToolsCall
       │    → payload.dialect=AgentDialect::Mcp
       │
       ├─ populatePayload(payload):
       │    case ToolsCall: payload.tool_name = params_name_ = "read_file"
       │
       ├─ payload.residual_params = residual_writer_->finalize()
       │    → PayloadRef::External{offset=0, len=120}  (full body in mmap arena, no copy)
       │
       ├─ params_byte_end_ > params_byte_start_:  ✓
       │    makeSubRef(payload.params_raw, params_byte_start_, plen, residual, JsonObject)
       │    → PayloadRef::External{mmap_offset + params_byte_start_, plen}  (no copy)
       │
       ├─ arguments_byte_end_ > arguments_byte_start_:  ✓ (Tier 1)
       │    makeSubRef(payload.arguments, arguments_byte_start_, alen, residual, JsonObject)
       │    → PayloadRef::External{mmap_offset + arguments_byte_start_, alen}  (no copy)
       │
       └─ capabilities: end==start → makeSubRef is a no-op
```

### Final `AgentPayload` state

```
payload.invocation      = AgentInvocation::ToolsCall
payload.dialect         = AgentDialect::Mcp
payload.tool_name       = "read_file"                        (plain std::string)
payload.arguments       = PayloadRef::External{off_A, len_A} (zero-copy mmap sub-range)
payload.params_raw      = PayloadRef::External{off_P, len_P} (zero-copy mmap sub-range)
payload.residual_params = PayloadRef::External{0, 120}       (full body in mmap arena)

request.jsonrpc_id  = "req-1"
request.rpc_method  = "tools/call"
```

No intermediate `params_buf_`, no `StringStreamWriter`, no `nlohmann` DOM, no
`ParamsSaxHandler`. All three sub-refs point into the same mmap region as
`residual_params`, created with pointer arithmetic only.

### Phase 4 — Auth and dispatch

`McpAuthFilter` inspects `request.rpc_method`, `payload.tool_name`, and
`request.principal` — plain `std::string` fields. No `PayloadStore` access occurs
during auth.

`prefetchExternalPayloadRefs` collects all `External` refs and fans out
`fetchAsync` calls. Page faults happen on detached threads, not the event loop.
After prefetch, all External refs are upgraded to `Buffered`; encoders call
`ref.toString()` with no mmap access. This pipeline is fully described in
PARSING.md ("Async External Payload Fetch") and is unchanged from the inference
path.

---

## Tier 2 trace — large `tools/call` body

For a body larger than `max_element_capture_bytes` (256 KB), `captureEnabled()`
returns `false`. The only difference from Tier 1:

- At the STRUCTURE POP for the arguments/capabilities container (depth 3),
  the byte-range recording is skipped.
- In `finish()`, `makeSubRef` for `payload.arguments` and `payload.capabilities`
  is a no-op (start == end == 0).
- `payload.params_raw` is still recorded and still created as a zero-copy sub-ref.
- `payload.tool_name` (or `params_uri_`, `params_ref_`) is still extracted.

Everything else — `feedChunk` loop, `str_target_` gating, `body_src_pos_` tracking,
`residual_writer_` streaming — runs identically. There is no code-path divergence
beyond the one `captureEnabled()` check.

---

## Memory analysis

### Heap cost by component

| Component | When active | `MmapPayloadStore` heap | Note |
|---|---|---|---|
| `dec_` (Wuffs decoder) | Entire parser lifetime | ~2 KB | One-time alloc, input-independent |
| `tok_data_[256]` | Entire parser lifetime | 2048 B in AgentBodyParser object | No separate alloc; 256 × 8 bytes |
| `residual_writer_` | First `feed()` to `finish()` | `MmapStreamWriter` metadata (~48 B) | Bytes go to mmap arena, not heap |
| `id_`, `method_` | After first feed() | O(field length) | Short in practice; bounded by max_body_bytes |
| `str_acc_` | Key accumulation | O(longest key at depth 1–2) | Cleared on each new key |
| `params_name_`, `params_uri_`, `params_ref_` | While in_params_ | O(field length) | Short routing strings |
| Depth-1 value (unknown field) | During string chain | **0** | str_target_=nullptr |
| Depth-2 value in params (unknown field) | During string chain | **0** | str_target_=nullptr |
| Depth-2 key in params | During string chain | O(key length) | Bounded by max_body_bytes |
| Depth-3+ anything | During string chain | **0** | str_target_=nullptr unconditionally |
| `PayloadRef::External` handles | After `finish()` | 12 bytes each | Pointers into mmap; body bytes in RSS |

### Peak heap by tier

| | Tier 1 (≤ 256 KB body) | Tier 2 (≤ 4 MB body) | Tier 3 (reject) |
|---|---|---|---|
| Wuffs decoder | ~2 KB | ~2 KB | n/a |
| Routing strings (id_, method_, params_name_, etc.) | < 1 KB total | < 1 KB total | n/a |
| str_acc_ (max) | < max_body_bytes | < max_body_bytes | n/a |
| Intermediate body buffer | **none** | **none** | ≤ one chunk |
| Large-string transient | **none** | **none** | n/a |
| **Total peak heap** | **~3 KB + O(keys)** | **~3 KB + O(keys)** | **≤ one chunk** |
| **Peak RSS (mmap)** | **Body size (evictable)** | **Body size (evictable)** | **0** |

There is no meaningful heap cost difference between Tier 1 and Tier 2 in the
Wuffs-based design. Both tiers run the same code and have the same peak heap.
The only difference is which `PayloadRef`s are populated.

### Old vs new: the 4 MB key attack

```
Attack body: {"method":"tools/call","params":{"A"*4MB: "value"}}

Old IncrementalJsonTokenizer (Tier 2 semantic mode):
  Tokenizer encounters the 4 MB key at depth 2 (inside params):
    state_ transitions to InKey
    token_buf_.append(c) × 4,000,000
    → token_buf_ grows to 4 MB on heap
    AgentHandler::onKey("A"*4MB) fires: handler ignores it
    → 4 MB is already allocated and then freed
  Attacker-controlled heap growth: O(4 MB)
  No configuration prevents this below max_body_bytes

New Wuffs-based AgentBodyParser (Tier 2):
  Wuffs emits ~62 STRING tokens of 65535 bytes each for the 4 MB key:
    For each token:
      first_in_group (or in_chain_=true for continuation):
        str_target_=&str_acc_  (keys at depth 2 are always accumulated)
      appendStringToken(str_acc_, raw, vbd) → str_acc_ grows by tlen bytes
  After all tokens: str_acc_ contains the full 4 MB key.
  params_key_ = str_acc_  (4 MB — same as old behavior for depth-2 keys)

  Critical difference:
  - Old design: depth-3+ keys also went into token_buf_ (4 MB if at depth 3)
  - Old design: values at depth 3+ also went into token_buf_ in semantic mode
  - New design: depth-3+ keys go into str_acc_ (same memory cost as depth-2)
  - New design: values at depth 3+ → str_target_=nullptr → 0 bytes, always

  For the specific attack (key at depth 2): same heap cost, same bound (max_body_bytes).
  For depth-3+ attacks (key or value): old design was vulnerable, new design: 0 bytes.
```

### Multimodal-style attack — large value at depth 3

```
Body: {"method":"tools/call","params":{"name":"foo","arguments":{"data":"<4 MB base64>"}}}

Old IncrementalJsonTokenizer (Tier 2):
  Tokenizer enters InStringValue at depth 3 for the 4 MB string.
  Before streaming-callback redesign: token_buf_ grew to 4 MB.
  After streaming-callback redesign: onStringChunk fires with str_target_=null → 0 heap.
  (The "streaming callbacks" in PARSING.md §"Old vulnerability" section fixed this
  partially, but it was still part of a fragile bespoke machine.)

New Wuffs-based AgentBodyParser:
  4 MB value at depth 3 arrives as ~62 Wuffs STRING COPY tokens.
  For each: str_target_=nullptr → appendStringToken not called → 0 heap.
  This is guaranteed by the str_target_ selection logic (depth 3+ → nullptr),
  not by a callback return value or a mode flag.
```

### Comparison: old IncrementalJsonTokenizer vs new Wuffs

| Property | Old `IncrementalJsonTokenizer` | New Wuffs-based |
|---|---|---|
| Key accumulation depth | All depths via `token_buf_` | Depth 1–2 via `str_acc_`; depth 3+ via `str_acc_` for keys (same, gated by max_body_bytes) |
| Value accumulation depth 3+ | Streaming callbacks (str_target_=null after redesign) | `str_target_=nullptr` unconditionally — never allocates |
| Parse state across HTTP chunks | 14 C++ enum states, manually preserved | Wuffs stackless coroutine, automatically resumed |
| Token size bound | Unbounded (whole string) per token event | 65535 bytes per Wuffs token (16-bit `tlen`) |
| Correctness guarantee | Bespoke state machine, no formal proof | Wuffs toolchain verifies memory safety |
| nlohmann dependency for agent path | Tier 1: yes (re-parse params_buf_ in finish()) | **None** — all fields extracted inline |
| `StringStreamWriter` / `params_buf_` | Tier 1: yes (heap std::string for params) | **None** — byte-range sub-ref of residual |
| `InCapture` mode for params | Tier 1: yes (tokenizer enters InCapture) | **None** — same Wuffs loop throughout |

---

## Invariants and security guarantees

### Invariant 1 — `body_src_pos_` is always exact

`body_src_pos_` advances by `tlen` for every token, including FILLER and DROP
tokens. The byte positions recorded for params and sub-container ranges are always
true offsets of those characters in the raw body stream that `residual_writer_` has
captured. `makeSubRef` can safely index into `residual_params` using these offsets
without any additional bookkeeping.

### Invariant 2 — Sub-refs are always valid subsets of residual

`makeSubRef` creates a sub-ref only when `end > start` and `residual` is non-empty.
`params_byte_start_` is set on STRUCTURE PUSH (opening delimiter); `params_byte_end_`
is set on STRUCTURE POP (one past the closing delimiter, because `body_src_pos_`
advanced through the closing character). Because `residual_writer_->append(chunk)`
runs before `feedChunk(chunk, ...)`, the full body including both delimiters is in
`residual_params` by the time `finish()` is called.

### Invariant 3 — `wuffs_done_` prevents double-processing

Once `wuffs_done_` is set (on OK status, end-of-data note, or any path that sets
it), all subsequent `feedChunk` calls return `OkStatus` immediately. This prevents
re-entry into the Wuffs coroutine after the document is complete. `finish()` calls
`feedChunk("", true)` to flush any trailing number token (numbers have no
terminator character); for a document that already returned OK this is a no-op.

### Invariant 4 — Duplicate-key detection is inline and early

`seen_jsonrpc_`, `seen_id_`, `seen_method_`, `seen_params_` are checked when a
STRING token chain completes as a depth-1 key. On the second occurrence, `has_error_`
is set and the inner token loop breaks immediately. The outer loop returns
`InvalidArgumentError` from `feedChunk`, which propagates through `feed()` to
`RequestDecoder::onData()`, which sends a 400 response before any auth or upstream
routing runs. The detection fires at parse time — as soon as the duplicate key's
closing quote is processed — not at `finish()` time.

### Invariant 5 — `str_target_` null-safety

`appendStringToken` is called only when `str_target_ != nullptr && tlen > 0`. The
null check is unconditional and precedes the call. `str_target_` is reset to
`nullptr` at string completion (the `if (!cont)` block). It cannot be left pointing
at a freed string object because the pointed-to strings (`id_`, `method_`,
`params_name_`, etc.) are data members that outlive all `feedChunk` calls.

### Invariant 6 — `in_chain_` spans `feed()` boundaries safely

If a STRING token chain spans an HTTP chunk boundary (Wuffs returns `short_read`
mid-chain), the next `feed()` call enters `feedChunk` with `in_chain_=true`.
The `first_in_group = !in_chain_` check correctly identifies the next token as a
continuation. `str_acc_` is not cleared and `str_target_` is not re-selected.
String accumulation continues from where it stopped.

### Security guarantee — bounded heap for attacker-controlled content

For any attacker-controlled body up to `max_body_bytes` (default 4 MB):

- **Depth 1 values**: only `id_` and `method_` accumulate. Both bounded by
  `max_body_bytes` (enforced in `feed()` before any parsing runs).
- **Depth 2 values (known fields)**: `params_name_`, `params_uri_`, `params_ref_`.
  Same bound.
- **Depth 2 values (unknown fields)**: `str_target_=nullptr`. Zero heap.
- **Depth 2 keys**: accumulate into `str_acc_`. Bounded by `max_body_bytes`.
- **Depth 3+ anything**: `str_target_=nullptr`. **Zero heap**. This is the key
  security property that the old design violated for depth-3+ keys.

Peak attacker-controlled heap allocation is bounded by `max_body_bytes`, enforced
before any parsing. The Wuffs token model adds a secondary bound: no single
operation allocates more than 65535 bytes per token.

---

## Build

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

The `AgentBodyParser` implementation in `request_decoder.cc` includes the Wuffs
header via the `ai_protocol_manager` build target. Wuffs requires no runtime
dependencies beyond the C standard library. `wuffs_json__decoder::alloc()` is the
only dynamic allocation Wuffs makes; all other operations are purely computational.

---

## Related documentation

- **PARSING.md** — full description of `InferenceBodyParser`,
  `IncrementalJsonTokenizer`, `PayloadStore`, `MmapPayloadStore`,
  `MmapStreamWriter`, async prefetch (`prefetchExternalPayloadRefs`), and the
  body-size tiering system that `AgentBodyParser` participates in alongside
  `InferenceBodyParser`. Components shared with the inference path — `residual_writer_`,
  `PayloadRef`, `makeSubRef`, the three-tier thresholds, and the dispatch pipeline —
  are documented in full there.

- `request_decoder.cc` lines 784–1189 — complete `AgentBodyParser` implementation.

- `request_decoder.h` — `DecoderConfig` field definitions (`max_body_bytes`,
  `max_element_capture_bytes`, `max_inline_bytes`).
