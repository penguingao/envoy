# Wuffs JSON: Closing the Last nlohmann Gap

## The gap

`PARSING.md` describes how the request decoder eliminated full-body buffering and
DOM allocation for both inference and agent request bodies. It explicitly flags one
remaining exception:

> *In Tier 1, `nlohmann` re-parses only `params_buf_` — the already-isolated params
> bytes, not the full body. **This is the only remaining use of `nlohmann` in the
> agent path.***

The Tier 1 path for `AgentBodyParser` uses `InCapture` mode to buffer the `params`
JSON object verbatim into a `StringStreamWriter`. At `finish()` time it calls
`nlohmann::json::parse(params_buf_)` to extract the routing sub-fields (`name`,
`uri`, `ref`, `arguments`, `capabilities`). The Wuffs integration replaces that
call.

---

## What nlohmann was doing there

`nlohmann::json::parse()` builds a full DOM tree from the captured bytes before any
routing field is accessible. For a typical Tier 1 `tools/call` params object like:

```json
{"name": "read_file", "arguments": {"path": "/etc/config.json"}}
```

The DOM allocation sequence is:

1. A `basic_json` root object node.
2. `std::string` keys for every field (`"name"`, `"arguments"`, `"path"`).
3. `std::string` value for `"read_file"`.
4. A child `basic_json` object for the `arguments` value, recursively expanding.

The caller then walks the DOM, extracts the three fields it actually needs, and
discards the entire tree. The DOM exists solely as a parsing intermediate; it is
never used after `finish()` returns.

**The eager-lexer problem compounds this.** Even if the DOM is small, nlohmann's
lexer allocates a `std::string` for every string value it encounters before firing
any SAX or DOM callback. A large `arguments` blob — or any deeply nested structure
with long string values — forces a transient allocation proportional to that
string's decoded length before the caller can decide whether it wants the value.
There is no way to intercept at the byte level through the public nlohmann API.

---

## What Wuffs does instead

`WuffsJsonObjectReader::read()` is a one-shot tokenizer loop. It fires one
callback per top-level key-value pair and returns. No DOM is built.

**String fields** (`name`, `uri`, `ref`): the Wuffs tokenizer emits raw byte slices
from the input buffer. `appendStringToken` appends only the decoded content bytes
into a `std::string` accumulator. The per-field allocation is proportional to the
field's decoded length, not to the entire params object.

**Container fields** (`arguments`, `capabilities`): Wuffs fires a STRUCTURE PUSH
token when a nested `{` or `[` opens. `WuffsJsonObjectReader` records the byte
offset of that token, waits for the matching STRUCTURE POP, and then slices
`input.substr(start, end - start)` — a `string_view` into the `params_buf_`
string that `AgentBodyParser` already owns. The container bytes are **not copied**;
`onObjectField` and `onArrayField` receive a zero-copy view.

`ParamsSaxHandler` then stores these container slices via `store_.store()` —
exactly one copy, directly into the `PayloadStore`, which is the same copy that
nlohmann's approach also required. The intermediate DOM copy is eliminated.

---

## Memory: before and after

For a Tier 1 `tools/call` body with `params_buf_` of size P and an `arguments`
object of size A:

| | nlohmann `json::parse` | Wuffs `WuffsJsonObjectReader` |
|---|---|---|
| DOM tree | O(P) heap — all nodes, all keys as strings | none |
| Eager-lexer string transient | O(A) heap — allocated before the handler sees it | none |
| `arguments` copy into store | O(A) heap — string copy from DOM node | O(A) heap — string copy from `string_view` slice |
| Wuffs decoder alloc | — | one small fixed-size heap alloc via `alloc()` |
| Token buffer | — | 256-slot stack array |
| `str_acc` for short string fields | — | O(field length), typically < 256 bytes |
| **Peak additional heap** | **O(P) + O(A)** | **O(A)** (store copy only) + negligible |

The DOM and the eager-lexer transient both disappear. The one remaining copy —
moving `arguments` bytes into the `PayloadStore` — was always necessary and is
unchanged.

---

## Why O(A) is not eliminated

`params_buf_` is a `std::string` that lives only for the duration of
`AgentBodyParser::finish()`. The `string_view` slices that `WuffsJsonObjectReader`
returns point into it. Before `params_buf_` is destroyed, those slices must be
copied into the `PayloadStore` via `store_.store(std::string(raw), ...)`.

If `params_buf_` were long-lived (e.g. stored as a member), the slice could become
an `External` ref into that buffer without any copy. In practice `params_buf_` is
a local that goes out of scope at the end of `finish()`, so the copy is required.
The cost is exactly O(A), which is also what nlohmann charged for the same
operation — the difference is that nlohmann added O(P) on top of it.

---

## Wuffs token model — what matters here

Wuffs emits `wuffs_base__token` values, each encoding a VBC (value_base_category),
VBD (value_base_detail), a `continued` flag, and a `length` in the source bytes.
The loop in `read()` advances a `src_pos` counter by `length` for every token,
including tokens that produce no output. This keeps `src_pos` synchronized with the
raw byte position in `input` at all times — which is what makes the zero-copy
container slice accurate.

### The DROP flag

For a JSON string like `"search"`, Wuffs emits three STRING-VBC tokens:

```
VBC=STRING  VBD=0x113 (DROP)  continued=1  length=1   ← opening "
VBC=STRING  VBD=0x203 (COPY)  continued=1  length=6   ← search
VBC=STRING  VBD=0x113 (DROP)  continued=0  length=1   ← closing "
```

`CONVERT_0_DST_1_SRC_DROP` means the source byte contributes one to `src_pos` but
zero bytes to the decoded string. The quote characters arrive as STRING tokens, not
FILLER. If `appendStringToken` does not check the DROP flag first, quote characters
are appended literally to the string accumulator and corrupt all subsequent key and
value comparisons. The check must be the first thing:

```cpp
if (vbd & WUFFS_BASE__TOKEN__VBD__STRING__CONVERT_0_DST_1_SRC_DROP) return;
```

### Container slice accuracy

`src_pos` advances for DROP tokens too, so it always reflects true byte positions.
When a STRUCTURE PUSH fires, `container_start = tok_start` records the position of
the opening `{` or `[`. When the matching STRUCTURE POP fires, the raw slice is:

```cpp
input.substr(container_start, src_pos - container_start)
```

This slice includes both delimiters and is exact regardless of how many DROP tokens
appeared inside the string fields within the container.

### Decoder allocation

The `wuffs_json__decoder` default constructor is deleted in C++ mode. Allocation
goes through `wuffs_json__decoder::alloc()`, which returns a `unique_ptr` with a
custom deleter. This is the only heap allocation that `WuffsJsonObjectReader::read()`
performs internally. The token buffer is stack-allocated:

```cpp
wuffs_base__token tok_data[256];
wuffs_base__token_buffer tok_buf = wuffs_base__slice_token__writer(
    wuffs_base__make_slice_token(tok_data, 256));
```

---

## Integration: `ParamsSaxHandler`

`ParamsSaxHandler` is a `WuffsJsonObjectReader::Handler` defined inside
`AgentBodyParser` in `request_decoder.cc`. It maps the five params fields to the
right `AgentPayload` members:

```cpp
void onStringField(absl::string_view key, std::string v) override {
    if      (key == "name") name_ = std::move(v);
    else if (key == "uri")  uri_  = std::move(v);
    else if (key == "ref")  ref_  = std::move(v);
}
void onObjectField(absl::string_view key, absl::string_view raw) override {
    if      (key == "arguments")    arguments_    = store_.store(std::string(raw), JsonObject);
    else if (key == "capabilities") capabilities_ = store_.store(std::string(raw), JsonObject);
}
void onArrayField(absl::string_view key, absl::string_view raw) override {
    if (key == "arguments") arguments_ = store_.store(std::string(raw), JsonArray);
}
```

The `populate(AgentPayload&)` method then routes the extracted values to the
correct payload fields based on `payload.invocation`, which must be set by the
second-pass classification in `finish()` before `populate()` is called.

`PayloadRef` has a deleted copy-assignment operator (it wraps `unique_ptr<Buffer::Instance>`).
`populate()` is non-`const` and uses `std::move(arguments_)` and
`std::move(capabilities_)`.

The call site in `finish()`:

```cpp
ParamsSaxHandler h(store_);
WuffsJsonObjectReader::read(params_json, h).IgnoreError();
h.populate(payload);
```

`IgnoreError()` is intentional. The outer `IncrementalJsonTokenizer` already
validated the JSON structure during streaming. A Wuffs parse error on the same
bytes would be spurious; the effect is "routing fields stay at zero-initialized
defaults," which is safe.

---

---

## End-to-end flow — Tier 1 `tools/call`

This traces a single Tier 1 agent request from arrival to a populated
`AgentPayload`, showing exactly what the Wuffs layer sees and does.

**Example request body** (small enough to be Tier 1, i.e. ≤ `max_element_capture_bytes`):

```json
{"jsonrpc":"2.0","id":"r1","method":"tools/call","params":{"name":"read_file","arguments":{"path":"/etc/config.json"}}}
```

### Phase 1 — Headers (`RequestDecoder::onHeaders`)

```
POST /mcp  Content-Type: application/json
  └─ classify() → ProtocolKind::AgenticMcp  (body needed to confirm invocation)
  └─ AgentBodyParser constructed:
       residual_writer_ = store_.beginStore(JsonObject)  ← MmapStreamWriter opened
       IncrementalJsonTokenizer initialized with AgentHandler
```

No Wuffs involvement yet.

### Phase 2 — Body chunks (`AgentBodyParser::feed`)

Each chunk runs two paths in parallel:

```
feed(chunk):
  total_bytes_ += chunk.size()           [stays ≤ max_element_capture_bytes → Tier 1]
  residual_writer_->append(chunk)        → full body streams into mmap arena
  chunk_base_ = total_bytes_ - chunk.size()
  tokenizer_.feed(chunk)
```

Inside `tokenizer_.feed()`, the 14-state machine fires `AgentHandler` callbacks:

```
[depth=1, key "jsonrpc"] → seen_jsonrpc_=true, current_key_="jsonrpc"
                           onStringStart/Chunk/End → str_target_=null (discarded)

[depth=1, key "id"]      → seen_id_=true, current_key_="id"
                           onStringStart → str_target_=&id_
                           onStringChunk("r1") → id_="r1"
                           onStringEnd

[depth=1, key "method"]  → seen_method_=true, current_key_="method"
                           onStringStart → str_target_=&method_
                           onStringChunk("tools/call") → method_="tools/call"
                           onStringEnd

[depth=1, key "params"]  → seen_params_=true, current_key_="params"

[depth=2, onStartObject] → current_key_=="params", captureEnabled()=true
                           → capturing_params_=true, params_capture_depth_=2
                           → tokenizer_.startCapture(params_writer_)
                                 state_=InCapture
                                 params_writer_.buf appended with '{'

[InCapture — every subsequent byte forwarded verbatim to params_writer_]:
  "name":"read_file","arguments":{"path":"/etc/config.json"}
  └─ each byte → params_writer_.buf.append(byte)
     no semantic events fire inside the captured container
     cap_depth_counter_ tracks nesting depth for '}' detection

[InCapture ends on matching '}']:
  cap_depth_counter_ drops to 0 on the outer '}'
  final '}' written to params_writer_.buf
  onEndObject() fires:
    capturing_params_=false
    params_captured_=true
    params_buf_ = std::move(params_writer_.buf)
       = {"name":"read_file","arguments":{"path":"/etc/config.json"}}
  tokenizer_ returns to semantic mode → ExpectCommaOrClose

[depth=1, outer '}'] → onEndObject, depth_=0
```

After `feed()` returns, `params_buf_` holds the exact captured bytes and
`method_` holds `"tools/call"`. The full body is in the mmap arena via
`residual_writer_`. No Wuffs involvement yet.

### Phase 3 — `AgentBodyParser::finish()`

This is the only phase where Wuffs runs.

```
tokenizer_.finish()      → validates document is complete, no error

request.jsonrpc_id = "r1"
request.rpc_method = "tools/call"

second-pass classify("tools/call"):
  payload.invocation = AgentInvocation::ToolsCall
  payload.dialect    = AgentDialect::Mcp

handler_.params_captured_ == true → Tier 1 branch:

  params_json = std::move(handler_.params_buf_)
    = {"name":"read_file","arguments":{"path":"/etc/config.json"}}

  ParamsSaxHandler params_handler(store_)
  WuffsJsonObjectReader::read(params_json, params_handler)
```

#### Wuffs decode loop — token by token

`read()` allocates the decoder via `wuffs_json__decoder::alloc()`, creates a
256-slot stack token buffer, and calls `decode_tokens` in a loop. The token
stream for `params_json` (src_pos advances for every token including DROP tokens):

```
src_pos  token                  VBC        VBD             cont  len   action
──────────────────────────────────────────────────────────────────────────────
0        {                      STRUCTURE  PUSH|TO_DICT    0     1     depth→1, is_dict[1]=true
                                                                        expecting_key[1]=true
1        "  (open quote)        STRING     DROP            1     1     in_chain=false→true
                                                                        str_acc.clear(), in_string=true
                                                                        string_is_key=true (depth=1,expecting_key)
                                                                        appendStringToken: DROP→return
2        name                   STRING     COPY            1     4     appendStringToken: COPY→str_acc="name"
6        "  (close quote)       STRING     DROP            0     1     appendStringToken: DROP→return
                                                                        continued=false → string complete
                                                                        string_is_key=true:
                                                                          current_key="name"
                                                                          expecting_key[1]=false
7        :                      FILLER     —               0     1     skip
8        "  (open quote)        STRING     DROP            1     1     in_chain=false→true
                                                                        str_acc.clear(), in_string=true
                                                                        string_is_key=false (expecting_key=false)
                                                                        appendStringToken: DROP→return
9        read_file              STRING     COPY            1     9     appendStringToken: COPY→str_acc="read_file"
18       "  (close quote)       STRING     DROP            0     1     appendStringToken: DROP→return
                                                                        continued=false → string complete
                                                                        string_is_key=false:
                                                                          handler.onStringField("name","read_file")
                                                                            → name_="read_file"
                                                                          expecting_key[1]=true
19       ,                      FILLER     —               0     1     skip
20       "  (open quote)        STRING     DROP            1     1     str_acc.clear, string_is_key=true
21       arguments              STRING     COPY            1     9     str_acc="arguments"
30       "  (close quote)       STRING     DROP            0     1     current_key="arguments", expecting_key[1]=false
31       :                      FILLER     —               0     1     skip
32       {                      STRUCTURE  PUSH|TO_DICT    0     1     depth=1, !expecting_key[1]:
                                                                          container_start=32
                                                                        depth→2, is_dict[2]=true, expecting_key[2]=true
33       "  (open quote)        STRING     DROP            1     1     depth=2 → str_acc/key handling for depth=2
                                                                          (depth≠1, so no top-level dispatch)
34       path                   STRING     COPY            1     4     str_acc accumulates (depth=2, not dispatched)
38       "  (close quote)       STRING     DROP            0     1     depth=2, no onStringField fired
39       :                      FILLER     —               0     1     skip
40       "  (open quote)        STRING     DROP            1     1     (inside nested object, depth=2)
41       /etc/config.json       STRING     COPY            1     16    (depth=2, not dispatched)
57       "  (close quote)       STRING     DROP            0     1
58       }  (close arguments)   STRUCTURE  POP|FROM_DICT   0     1     depth==2, container_start=32 > 0:
                                                                          raw = params_json[32..59) = {"path":"/etc/config.json"}
                                                                          was_dict=true:
                                                                            handler.onObjectField("arguments", raw)
                                                                              → arguments_ = store_.store(std::string(raw), JsonObject)
                                                                          container_start=0
                                                                          depth→1, expecting_key[1]=true
59       }  (close params root) STRUCTURE  POP|FROM_DICT   0     1     depth==1 → exit, depth→0

decode_tokens returns nullptr (done) → read() returns OkStatus
```

#### `params_handler.populate(payload)`

```
payload.invocation == AgentInvocation::ToolsCall:
  payload.tool_name  = name_       = "read_file"
  payload.arguments  = std::move(arguments_)   [PayloadRef into store]
```

#### Back in `finish()`

```
params_handler.populate(payload)
payload.params_raw = store_.store(std::move(params_json), PayloadKind::JsonObject)
  → params_json moved into store; payload.params_raw = PayloadRef for full params bytes

residual_writer_->finalize()
  → payload.residual_params = PayloadRef::External{0, 120}  [full body in mmap]
```

### Final `AgentPayload` state

```
payload.invocation      = AgentInvocation::ToolsCall
payload.dialect         = AgentDialect::Mcp
payload.tool_name       = "read_file"
payload.arguments       = PayloadRef → {"path":"/etc/config.json"}  (in store)
payload.params_raw      = PayloadRef → {"name":"read_file","arguments":{"path":"/etc/config.json"}}
payload.residual_params = PayloadRef::External{0, 120}  (full body in mmap arena)

request.jsonrpc_id = "r1"
request.rpc_method = "tools/call"
```

### What Wuffs added vs. what came before

| Step | Before (nlohmann) | After (Wuffs) |
|---|---|---|
| Parse `params_buf_` | `nlohmann::json::parse()` → DOM tree: nodes for every key/value inside `arguments` | `WuffsJsonObjectReader::read()` → token loop, no DOM |
| Extract `"name"` | DOM walk: `root["name"].get<std::string>()` | `onStringField("name", "read_file")` callback |
| Extract `"arguments"` | DOM walk: re-serialize child object → `std::string` copy | `onObjectField("arguments", raw)` where `raw` is a zero-copy `string_view` into `params_buf_` |
| Copy into store | `store_.store(json["arguments"].dump())` — dump allocates | `store_.store(std::string(raw), ...)` — one copy from the slice |
| Heap at this step | O(P) DOM + O(A) re-serialized string | O(1) decoder alloc + O(A) one store copy |

---

## Deep memory analysis

### Heap vs RSS

Before counting allocations, it matters which kind of memory is being spent:

**Heap** — memory returned by `malloc`/`new`. Non-evictable. Directly competes with
every other allocation in the process.

**RSS** — all physical pages currently mapped in, including heap, stack, code, and
mmap regions. mmap pages are backed by the OS page-cache: the kernel can evict them
silently under pressure and reload them from the file descriptor on next access.
An RSS spike from mmap does not starve the allocator.

All figures below track heap unless explicitly noted as RSS.

---

### Components from `PARSING.md` that are still active

The Wuffs change is surgical. Every other component documented in `PARSING.md`
continues to operate exactly as before.

#### `StringStreamWriter` (heap accumulator for `params_buf_`)

`StringStreamWriter` is a minimal `StreamWriter` whose `append()` grows a `std::string`:

```cpp
struct StringStreamWriter : public StreamWriter {
    std::string buf;
    void append(absl::string_view bytes) override { buf.append(bytes); }
    PayloadRef finalize() override { return PayloadRef{}; }  // unused
};
```

In Tier 1, when the tokenizer enters `InCapture` on the `params` opening `{`,
`startCapture(params_writer_)` is called. From that point every byte of the params
container is forwarded to `params_writer_.buf`. This is O(P) on the heap, where P
is the params object byte length. The string stays live until `finish()` moves it
out via `params_json = std::move(handler_.params_buf_)`. Wuffs then reads from
`params_json` and the string is finally consumed by `store_.store(std::move(params_json), ...)`.

This component is unchanged by the Wuffs integration. The only difference is what
happens to the string *after* it is moved into `params_json`.

#### `MmapStreamWriter` / `residual_writer_` (full-body streaming)

`residual_writer_` is opened on the first `feed()` call and writes every incoming
byte into the mmap arena. It is still open when `finish()` runs the Wuffs path —
the call sequence is:

```
finish():
  tokenizer_.finish()
  ...
  WuffsJsonObjectReader::read(params_json, params_handler)   ← Wuffs runs here
  params_handler.populate(payload)
  payload.params_raw = store_.store(std::move(params_json), ...)
  ...
  payload.residual_params = residual_writer_->finalize()     ← finalized after Wuffs
```

The writer struct itself is small (< 1 KB heap). The bytes it has accumulated are
in the mmap arena (RSS, not heap). Wuffs sees `params_json` — a separate `std::string`
— not the mmap region. There is no interaction between the Wuffs scan and the
residual writer.

#### `PayloadRef::External` and `PayloadRef::Inline`

Every `PayloadStore::store()` call in the Wuffs path produces a `PayloadRef`:

| Call site | Input size | `MmapPayloadStore` result | `InMemoryPayloadStore` result |
|---|---|---|---|
| `onObjectField("arguments", raw)` | O(A) — the `arguments` bytes | `Inline` if A ≤ `max_inline_bytes`, else `External{offset, A}` | `Inline` or `Buffered(OwnedImpl)` |
| `store_.store(std::move(params_json), ...)` | O(P) — the full params bytes | `Inline` if P ≤ `max_inline_bytes`, else `External{offset, P}` | `Inline` or `Buffered(OwnedImpl)` |
| `residual_writer_->finalize()` | O(B) — the full body | `External{0, B}` (always, for production bodies) | `Buffered(OwnedImpl)` |

`External` refs carry only 12 bytes on heap (`uint64_t offset` + `size_t length`).
The bytes they point to live in the mmap arena (RSS).

#### Async prefetch pipeline

Any `External` refs produced by the Wuffs path — `arguments`, `params_raw`,
`residual_params` — flow through `prefetchExternalPayloadRefs` before dispatch,
exactly as documented in `PARSING.md`. A detached thread calls `pread()` for each
External ref, marshaling the result buffer back to the event loop via
`dispatcher.post()`. Encoders see only `Buffered` refs by the time they run.

No change here from the pre-Wuffs design. The External refs produced are the same
refs (same offsets, same lengths) — only the code path that created `params_raw`
and `arguments` differs.

---

### Heap live during each phase of a Tier 1 agent request

Using the concrete example body B (120 bytes), params P (64 bytes), arguments A (28 bytes):

```
Phase 1 — onHeaders:
  AgentBodyParser members (stack + handler struct)    < 1 KB  heap
  MmapStreamWriter struct (residual_writer_)          < 1 KB  heap
  mmap arena                                              0   RSS (grows as bytes arrive)

Phase 2 — feed():
  params_writer_.buf (StringStreamWriter)        0 → O(P)   heap  (grows during InCapture)
  MmapStreamWriter struct                             < 1 KB  heap
  mmap arena                                      0 → O(B)   RSS   (grows per chunk)
  token_buf_ (keys, numbers, keywords only)           < 1 KB  heap
  handler scalars (id_, method_)                      < 1 KB  heap

  ─── peak during feed(): O(P) [params_buf_] + O(B) RSS ───

Phase 3 — finish(), before Wuffs:
  params_json (moved from params_buf_)                O(P)   heap  (same allocation, no copy)
  MmapStreamWriter struct (still open)                < 1 KB  heap
  mmap arena                                          O(B)   RSS

  Wuffs:
    wuffs_json__decoder (heap alloc via alloc())      ~2 KB  heap  [fixed, independent of input size]
    tok_data[256] (stack)                             ~2 KB  stack
    str_acc (string accumulator for field values)     < 1 KB  heap  (reused per string field)
    name_ / uri_ / ref_ in ParamsSaxHandler           < 1 KB  heap  (O(field length), bounded)
    std::string(raw) for arguments                    O(A)   heap  [transient — briefly simultaneous
                                                                    with params_json]
      → store_.store(std::move(...)) absorbs it:
          MmapPayloadStore: memcpy to mmap, string freed  → heap drops back by O(A)
          InMemoryPayloadStore: OwnedImpl stays on heap

  ─── peak during Wuffs (MmapPayloadStore):
        O(P) [params_json] + O(A) [arguments transient] + ~3 KB constants ───

  After Wuffs, params_raw store:
    store_.store(std::move(params_json), ...):
      MmapPayloadStore: params_json moved into store, memcpy to mmap, string freed
      InMemoryPayloadStore: params_json moved into OwnedImpl

  residual_writer_->finalize():
    MmapPayloadStore: PayloadRef::External{0, B}  (8 bytes heap, B bytes RSS)
    InMemoryPayloadStore: PayloadRef::Buffered(OwnedImpl, B bytes heap)

  ─── peak during finish(), end (MmapPayloadStore):
        ~3 KB heap + O(B) RSS ───
```

---

### Old vs new: precise heap peak comparison

For a Tier 1 agent request. Variables: B = full body, P = params bytes, A = arguments bytes.

#### Old design (nlohmann DOM)

```
feed():
  params_buf_ grows to O(P)           [StringStreamWriter — unchanged]
  residual_writer_ writes to mmap     [unchanged]

finish() — Tier 1 branch:
  params_json = std::move(params_buf_)         O(P) heap

  nlohmann::json::parse(params_json):
    DOM tree constructed from params_json       O(P) heap  ← simultaneous with params_json
    peak: O(P) [params_json] + O(P) [DOM] = O(2P) heap simultaneously

  json["arguments"].dump():
    re-serializes the arguments subtree         O(A) heap  ← transient for store call
    store_.store(...) absorbs it

  DOM destroyed
  store_.store(params_json, ...) → copies params_json into store, params_json freed
```

Peak heap during `finish()`: **O(2P)** — params_json and the nlohmann DOM live simultaneously.
For `InMemoryPayloadStore` add O(B) for the residual OwnedImpl: **O(B + 2P)**.

#### New design (Wuffs)

```
feed():
  params_buf_ grows to O(P)           [StringStreamWriter — identical to old]
  residual_writer_ writes to mmap     [identical to old]

finish() — Tier 1 branch:
  params_json = std::move(params_buf_)         O(P) heap

  WuffsJsonObjectReader::read(params_json, h):
    wuffs_json__decoder::alloc()               ~2 KB heap  (fixed, independent of P)
    tok_data[256] on stack
    str_acc: O(longest string field value)     < 1 KB heap (reused, cleared per field)

    onObjectField("arguments", raw):
      std::string(raw)                         O(A) heap  ← transient, simultaneous with params_json
      store_.store(std::move(...)):
        MmapPayloadStore: memcpy to arena, string freed
        peak: O(P) + O(A) + ~3 KB ≤ O(P + A) ≤ O(2P)

  store_.store(std::move(params_json), ...):
    MmapPayloadStore: memcpy to arena, params_json freed
    heap drops to ~3 KB constants
```

Peak heap during `finish()`: **O(P + A)** where A ≤ P, so peak ≤ O(2P) but typically much less.
For `InMemoryPayloadStore` add O(B) for the residual OwnedImpl: **O(B + P + A)** vs. old O(B + 2P).

| | nlohmann (old) | Wuffs (new) |
|---|---|---|
| DOM tree | O(P) — all nodes, keys, values | none |
| Eager string transient per field | O(field_len) per `.dump()` call | none |
| Arguments re-serialization | O(A) — `.dump()` re-encodes the subtree | O(A) — `std::string(raw)` copy from slice |
| params_json simultaneous with DOM | yes — O(P) + O(P) simultaneous | no DOM — O(P) params_json only |
| Peak heap (MmapPayloadStore) | **O(2P)** | **O(P + A)** |
| Peak heap (InMemoryPayloadStore) | **O(B + 2P)** | **O(B + P + A)** |
| Wuffs decoder alloc | — | ~2 KB (fixed, not input-proportional) |

The reduction is P − A ≥ 0 in heap, which is the DOM tree that no longer exists.
For a 10 KB params object with a 8 KB arguments subtree, that is a reduction from 20 KB
to 18 KB. For a 256 KB params object at the Tier 1 ceiling with a 200 KB arguments blob,
it is a reduction from 512 KB to 456 KB — the DOM tree for a 256 KB JSON object is
non-trivial to allocate and GC.

---

### `MmapPayloadStore` vs `InMemoryPayloadStore` — per-phase heap

| Phase | Allocation | `MmapPayloadStore` | `InMemoryPayloadStore` |
|---|---|---|---|
| `feed()` streaming | `residual_writer_` bytes | O(B) RSS (mmap arena) | O(B) heap (`OwnedImpl`) |
| `feed()` InCapture | `params_writer_.buf` | O(P) heap (StringStreamWriter, both backends) | O(P) heap |
| `finish()` Wuffs decoder | `wuffs_json__decoder` | ~2 KB heap | ~2 KB heap |
| `finish()` arguments store | `std::string(raw)` transient | O(A) heap briefly → freed after `memcpy` to arena | O(A) heap → stays in `OwnedImpl` |
| `finish()` params_raw store | `std::move(params_json)` | O(P) heap → freed after `memcpy` | O(P) heap → stays in `OwnedImpl` |
| `finish()` residual finalize | `PayloadRef::External` handle | 12 bytes heap; O(B) RSS | `OwnedImpl` O(B) heap (already live) |
| Peak (all simultaneous) | — | O(P) + O(A) + ~3 KB + O(B) RSS | O(B) + O(P) + O(A) + ~3 KB |

The qualitative difference between the backends is entirely in whether large field bytes
count against heap (InMemory) or RSS (Mmap). This is unchanged from the pre-Wuffs design.
What Wuffs changes is the O(P) DOM term that no longer appears in either column.

---

### Why `params_buf_` must stay heap-allocated

`params_buf_` cannot be redirected into the mmap arena (like `residual_writer_` does)
because `WuffsJsonObjectReader` needs a contiguous `string_view` over it. The mmap arena
is a bump allocator that other writers may interleave with, and there is no guarantee
of contiguity after a capacity-doubling `mremap`. `StringStreamWriter` keeps the bytes
in a single `std::string` precisely so `read()` can take a single `string_view` over the
whole params object without any copy.

If params objects were consistently small — which they are for all known MCP invocations —
the O(P) cost is dominated by the arena and is not a practical concern. If a future
invocation type has multi-MB params, the Tier 2 path (semantic streaming without
`StringStreamWriter`) is the correct answer, not a streaming Wuffs scan.

---

## Build

The amalgamated file `wuffs-v0.4.c` doubles as header and implementation.
`WUFFS_IMPLEMENTATION` must be defined in exactly one compilation unit:

| File | Role |
|---|---|
| `wuffs_impl.c` | Defines `WUFFS_IMPLEMENTATION`, compiled as C to avoid C++-only warnings in the generated code |
| `wuffs_json.cc` | Includes `wuffs-v0.4.c` without `WUFFS_IMPLEMENTATION` (declarations only) |

In `codec/BUILD`, `wuffs_json_lib` lists both files as `srcs`. The upstream
`@wuffs//:wuffs` target declares `wuffs-v0.4.c` as `hdrs` (not `srcs`) so that
every consumer sees the declarations regardless of which file includes it first.
`@nlohmann_json` is removed from `request_decoder_lib`'s `deps`.
