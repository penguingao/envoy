# Request Decoder & Payload Store — AI Protocol Manager Codec

## Motivation

AI inference and agentic request bodies are structurally unlike typical REST
payloads. A single `POST /v1/chat/completions` may carry dozens of conversation
turns, each potentially containing base64-encoded images or large tool schemas,
pushing the JSON body into the hundreds of kilobytes. Under concurrent load the
naive approach — buffer the full body, parse it into a DOM, copy each field into a
`std::string` — creates two compounding problems:

**Problem 1 — full-body buffer before parse**: `nlohmann::json::parse()` and even
`nlohmann::json::sax_parse()` require the entire body to be present in memory
before any event fires, because their internal parse state lives on the C++ call
stack (recursive descent). There is no way to pause and resume across HTTP chunk
boundaries. For a 200 KB body across thousands of concurrent streams this means
hundreds of megabytes of heap just to hold transient input, all of which becomes
garbage the moment fields are extracted.

**Problem 2 — field copies as heap strings**: Once parsed, each `messages[]`
element and `tools[]` definition is typically stored as a re-serialized
`std::string`. Multiple filters may then each hold their own copy, multiplying the
RSS impact and fragmenting the allocator under high concurrency.

Two complementary designs eliminate both problems:

1. **Incremental tokenizer** (`IncrementalJsonTokenizer`) — all parse state lives
   in data members (not the call stack), so the tokenizer can be suspended after
   any byte and resumed on the next chunk. Body bytes stream directly from
   `onData()` into the parse engine; large sub-documents (`messages[]` elements,
   `tools[]` entries, `params`) are captured via `StreamWriter` sessions that write
   raw bytes directly into the `PayloadStore` as they arrive. In Tier 1 (body ≤
   256 KB) peak heap is O(1) — one chunk plus small handler state. In Tier 2
   (larger bodies) the body moves off heap into evictable mmap page-cache, though
   `token_buf_` still scales with the largest string value in the document.

2. **Payload store** (`PayloadRef` + `PayloadStore`) — separates the *handle* to a
   field value from its *storage*. Small fields stay inline in process memory for
   zero-overhead access; large fields are offloaded to an mmap-backed temp file so
   the kernel can evict pages under memory pressure. Filters share `PayloadRef`
   handles — lightweight `{offset, length}` pairs — rather than copying bytes.

---

## Request Decoder

### Overview

`RequestDecoder` (`request_decoder.h/cc`) translates a streamed HTTP request into
a fully-populated `AiRequest`. It is driven by the outer Envoy filter's
`decodeHeaders` / `decodeData` / `decodeTrailers` callbacks.

```
onHeaders()    — classify protocol, init body parser and StreamWriters
onData()       — feed each chunk directly into the incremental tokenizer
onEndStream()  — finalize StreamWriters, validate result, populate AiRequest
take()         — move completed AiRequest out of the decoder
```

`onData()` is where parsing happens. Each HTTP chunk is fed byte-by-byte into the
tokenizer. Scalar fields (`model`, `stream`, `temperature`, etc.) are extracted
immediately as events fire. Large sub-documents open a `StreamWriter` session on
the `PayloadStore`; subsequent raw bytes are written directly into the store as
they arrive, with no intermediate accumulation.

### State machine

```
AwaitingHeaders
  │
  ├─ GET/DELETE (no body expected) ──→ BodilessComplete ──→ BodyComplete
  │
  ├─ POST/PUT (Inference body) ──→ ParsingInferenceBody ──→ BodyComplete
  │
  └─ POST (Agent/JSON-RPC body) ──→ ParsingAgentBody ──→ BodyComplete
```

---

## `IncrementalJsonTokenizer` — streaming JSON parser

### Design

`IncrementalJsonTokenizer` is a custom 14-state machine whose entire parse state
lives in data members. Because no state is held on the C++ call stack, the
tokenizer can process an arbitrary number of bytes from a chunk via `feed()`, stop
at the end of the chunk, and resume exactly where it left off when the next chunk
arrives.

```
State members (abbreviated):
  ParseState state_       — current token state (14 values)
  int        depth_       — JSON nesting depth
  std::string token_buf_  — accumulating string/number token
  bool        in_key_     — true while lexing a JSON key
```

The core entry point is `processByte(uint8_t c, bool& reprocess)`. It advances
the state machine by one byte, firing handler callbacks when a complete token is
ready. Setting `reprocess = true` instructs `feed()` to replay the same byte
through the new state (used when a delimiter both ends one token and begins the
next, e.g. a `}` following a number literal — the `}` terminates the number and
must then be reprocessed as an object close).

### 14 parse states

| State | Description |
|---|---|
| `Root` | Before any token; skip whitespace, dispatch on first non-WS byte |
| `InString` | Inside a quoted string or key |
| `InEscape` | Immediately after `\` inside a string |
| `InNumber` | Building a numeric literal (integer or float) |
| `InTrue` / `InFalse` / `InNull` | Building keyword literals |
| `ExpectColon` | Between object key and value |
| `ExpectValue` | After `:` or after `[`, expecting a value |
| `ExpectCommaOrClose` | After a complete value; awaiting `,`, `}`, or `]` |
| `InCapture` | Raw-byte forwarding mode (see below) |
| `Done` | Top-level value complete; extra input is an error |
| `Error` | Unrecoverable parse error |

### Capture mode — zero-copy element streaming

When an `InferenceHandler` or `AgentHandler` encounters a `messages[]` element,
`tools[]` entry, or `params` object, it calls `tokenizer_.startCapture(writer)`.
This switches the state machine into `InCapture` mode.

In capture mode:

- **Every subsequent byte is forwarded verbatim** to `writer.append()`, which
  writes directly into the active `MmapStreamWriter` session (or
  `InMemoryStreamWriter` for tests). No semantic events fire inside the captured
  container.
- A lightweight secondary counter (`cap_depth_counter_`) and two boolean flags
  (`cap_in_string_`, `cap_in_escape_`) track nesting and string context — just
  enough to detect the matching close brace or bracket. No tokenization or
  allocation occurs for any value inside the capture.
- When `cap_depth_counter_` drops to zero (the matching `}` or `]` is seen), the
  byte is still forwarded to the writer, `capture_writer_` is cleared, `depth_` is
  decremented, and the normal `onEndObject()` / `onEndArray()` callback fires. The
  tokenizer resumes normal operation from `ExpectCommaOrClose`.

```
normal token events:
  InferenceHandler::onStartObject()  [depth=3, in_messages_/in_tools_]
    └─ elem_writer_ = store_.beginStore(PayloadKind::MessageElement)
    └─ tokenizer_.startCapture(*elem_writer_)   ← enters InCapture

InCapture bytes (all going to elem_writer_):
  {"role":"user","content":"hello"}
  └─ each byte → elem_writer_->append({&c, 1})

cap_depth_counter_ == 0 on `}`:
  └─ elem_writer_->finalize() → PayloadRef
  └─ payload_.messages.push_back(ref)
  └─ state_ = ExpectCommaOrClose, resume normal tokenization
```

This is strictly cheaper than the previous byte-range capture approach:

| | Old (byte-range slicing) | New (InCapture streaming) |
|---|---|---|
| Element bytes copied | twice (slab → heap string in store) | once (chunk → mmap region via writer) |
| Intermediate allocation | one `reserve`d string per element | none |
| Parser work per byte inside element | depth counter branch | depth counter + 1 `append()` call |
| Requires full body before capture | yes (sliceBuffer needs full slices) | no (bytes stream in per chunk) |

### `Handler` interface

`IncrementalJsonTokenizer` calls into a `Handler` abstract class:

```cpp
struct Handler {
  virtual bool onKey(absl::string_view key)  = 0;
  virtual bool onString(absl::string_view v) = 0;
  virtual bool onInt(int64_t v)              = 0;
  virtual bool onFloat(double v)             = 0;
  virtual bool onBool(bool v)                = 0;
  virtual bool onNull()                      = 0;
  virtual bool onStartObject()               = 0;
  virtual bool onEndObject()                 = 0;
  virtual bool onStartArray()                = 0;
  virtual bool onEndArray()                  = 0;
};
```

Returning `false` from any callback aborts the parse with an `Internal` error.

### Public API

```cpp
// Feed a chunk. May be called any number of times before finish().
absl::Status feed(absl::string_view chunk);

// Signal end of input. Returns error if the document is incomplete.
absl::Status finish();

// Call during an onStartObject/Array callback to begin raw-byte capture.
// All subsequent bytes go to writer.append() until the matching close.
void startCapture(StreamWriter& writer);
```

---

## `InferenceBodyParser` — OpenAI REST body parsing

Handles `POST /v1/chat/completions`, `/v1/completions`, `/v1/embeddings`, etc.

### Construction

At construction, `InferenceBodyParser` opens a `StreamWriter` for `residual_params`
immediately via `store_.beginStore(PayloadKind::ResidualParams)`. Every byte that
arrives in `feed()` is appended to this writer, so by the time `finish()` is
called the complete body has been streamed to the store without any intermediate
`body_buffer_` accumulation.

### `InferenceHandler` depth table

| Depth | Key | Action |
|---|---|---|
| 1 | `model` | Extracts into `request_.model` |
| 1 | `stream` | Extracts into `request_.stream` |
| 1 | `temperature`, `top_p`, `max_tokens`, `n`, `seed`, `stop` | Extracts into `sampling_` |
| 2 | `messages` or `tools` | Sets `in_messages_` / `in_tools_` flag |
| 3 | opening `{` or `[` inside `messages`/`tools` | Opens `elem_writer_ = store_.beginStore(...)`, calls `tokenizer_.startCapture(*elem_writer_)` — only when `captureEnabled()` |
| after InCapture ends | — | `elem_writer_->finalize()` → `PayloadRef`; push into `messages_` or `tools_` list |
| all other keys/values | — | Ignored (byte-level captured in `residual_params` anyway) |

### Soft-limit gating (`captureEnabled`)

`captureEnabled()` returns `total_bytes_ <= config_.max_element_capture_bytes`.
This is evaluated per-chunk via a running counter `total_bytes_`. When the check
returns `false`, element writers are never opened: `startCapture()` is not called,
so the tokenizer stays in normal mode and the bytes land only in the
`residual_writer_` stream. Scalar extraction at depth 1 is unaffected.

### Auth ordering

Extracted fields accumulate in `InferenceHandler` members (`model_`, `stream_`,
`sampling_`, `messages_`, `tools_`) during `feed()` calls. They are only moved
into the real `InferencePayload` and `AiRequest` inside `finish()`, which is
called from `onEndStream()` — after any body-covering authentication upstream in
the filter chain has completed. Parsing runs incrementally but results are not
acted upon until auth is done.

---

## `AgentBodyParser` — JSON-RPC 2.0 body parsing

Handles MCP and A2A agent requests.

### `AgentHandler` field table

| Field | Extraction |
|---|---|
| `id` | Stored as `request_.jsonrpc_id` (string or integer) |
| `method` | Stored as `request_.rpc_method`; triggers re-classify in `finish()` |
| `params` | When `captureEnabled()`, opens a `StringStreamWriter` (a trivial `std::string` accumulator) and calls `tokenizer_.startCapture(writer)`; raw bytes accumulate in the string for later `json::parse()` in `finish()` to populate `AgentPayload` fields |

Two classification passes:
1. **Headers-time** (`onHeaders`): classify by HTTP method + path alone (no body yet).
2. **Body-time** (`AgentBodyParser::finish`): once `rpc_method` is known, re-classify
   to determine the specific `AgentInvocation` enum value.

---

## Body-size tiering — three-tier memory strategy

### Three tiers

`DecoderConfig` exposes two thresholds that carve the body-size space into three
tiers:

```
┌───────────────────────────────────────────────────────────────────────────┐
│ body size          │ behavior                                             │
├───────────────────────────────────────────────────────────────────────────┤
│ ≤ max_element_     │ Tier 1 — full capture                                │
│   capture_bytes    │ captureEnabled() true. messages[]/tools[] elements   │
│ (default 256 KB)   │ are streamed into individual PayloadRefs via         │
│                    │ StreamWriter sessions. params → params_raw +          │
│                    │ routing fields populated.                             │
├───────────────────────────────────────────────────────────────────────────┤
│ > max_element_     │ Tier 2 — scalars only                                │
│   capture_bytes    │ captureEnabled() false. Element/params capture       │
│ ≤ max_body_bytes   │ skipped. Top-level scalars (model, stream,           │
│ (default 4 MB)     │ temperature, max_tokens, id, method) still           │
│                    │ extracted. messages[]/tools[] left empty.            │
│                    │ params_raw empty; tool_name/resource_uri not set.    │
├───────────────────────────────────────────────────────────────────────────┤
│ > max_body_bytes   │ Tier 3 — hard reject                                 │
│ (default 4 MB)     │ feed() returns ResourceExhausted immediately.        │
│                    │ No bytes are stored beyond this ceiling.             │
└───────────────────────────────────────────────────────────────────────────┘
```

### Implementation

**Hard limit — per-chunk in `feed()`**

Both `InferenceBodyParser::feed()` and `AgentBodyParser::feed()` check before
processing each chunk:

```cpp
if (total_bytes_ + chunk.size() > config_.max_body_bytes) {
    return absl::ResourceExhaustedError(...);
}
```

The error propagates through `onData()` to the filter, which can immediately
return a 413 response.

**Soft limit — per-chunk via `captureEnabled()`**

`captureEnabled()` is evaluated each time a handler would otherwise open a capture
session. Because `total_bytes_` is updated before each `feed()` call, the
threshold comparison is always current:

```cpp
bool captureEnabled() const {
  return total_bytes_ <= config_.max_element_capture_bytes;
}
```

When `captureEnabled()` returns `false`, element writers are never opened and
`startCapture()` is never called. The tokenizer processes all depth-3 events
normally (incrementing/decrementing `depth_`), but no byte-level data is captured
for those elements. Scalar extraction at depth 1 is entirely unaffected.

### Memory characteristics per tier

| | Tier 1 (full capture) | Tier 2 (scalars only) | Tier 3 (reject) |
|---|---|---|---|
| residual_writer_ | streams full body to store | streams full body to store | partial (rejected mid-chunk) |
| Intermediate element buffer | none (bytes go directly to elem_writer_ via InCapture) | none (no element capture; token_buf_ holds current string token) | n/a |
| Store copies | Σ element/params bytes as External refs | none | none |
| Peak heap | O(1) — token_buf_ bounded by depth-1 scalars | ≈ largest string value anywhere in body (token_buf_) | ≤ one chunk |
| Peak RSS | B + Σ elements (evictable mmap) | B (evictable mmap) + token_buf_ heap | ≤ one chunk |

See [Peak Memory Analysis](#peak-memory-analysis) for the full breakdown by tier,
including the multimodal worst-case.

---

## Peak Memory Analysis

This section analyses both **heap** (process allocator, shows in `jemalloc` /
`ASAN`) and **RSS** (total resident physical memory, including mmap pages).

The mmap arena (`MAP_SHARED` backed by the unlinked temp file) is not heap — its
pages are kernel page-cache that the OS can evict under memory pressure
independently of `malloc`. They do count toward RSS once written to, because a
write page-fault makes the page physically resident.

### Old approach — nlohmann SAX

#### The eager-lexer problem

`nlohmann::json::sax_parse()` fully tokenizes each JSON value before calling any
SAX callback. For string values this means allocating a `std::string` for the
fully-unescaped content before `string()` is called — regardless of whether the
handler keeps the value or immediately discards it. There is no way to intercept
at the byte level through the public SAX API.

For a multimodal inference request carrying a 10 MB base64-encoded image:

```json
{
  "messages": [{
    "role": "user",
    "content": [{"type": "image_url", "image_url": {"url": "data:image/jpeg;base64,<10MB>"}}]
  }]
}
```

Even with `capturing_element_ = true` (which makes the handler return `true`
immediately), nlohmann has already allocated a ~13.3 MB `std::string` for the
unescaped base64 value before the `string()` callback fires. That string is
purely transient — it exists for exactly one callback invocation and is then
destroyed.

#### Precise peak sequence (old approach, Tier 1 element capture)

The transient (`string()` callback) and the `sliceBuffer` result (element close)
are **sequential, not simultaneous**. `sliceBuffer` only runs when the element's
closing `}` is reached — after the transient for every value inside the element
has already been freed:

```
time →

[sax_parse encounters base64 string at depth ~6]
  heap: body_buffer_ (B) + nlohmann transient (S)   ← peak A = B + S
  transient freed immediately after string() returns

[sax_parse encounters messages[0] closing `}` at depth 3]
  heap: body_buffer_ (B) + sliceBuffer result (E)   ← peak B = B + E
  sliceBuffer result handed to store_.store():
    MmapPayloadStore: memcpy to mmap, string freed → heap drops back to B
```

Where `B` = body size, `S` = largest string value in the body, `E` = element size.
For the single-message multimodal case, `S ≈ E ≈ B`, so both peaks are ≈ 2× body.
They do not stack: **peak heap ≈ body + max(largest string value, largest element)**.

For Tier 2 (no element capture), `sliceBuffer` is never called, leaving only:
**peak heap ≈ body + largest string value**.

| | Tier 1 (≤256 KB body) | Tier 2 (≤4 MB body) | Tier 3 (reject) |
|---|---|---|---|
| Heap | `B` + max(`S`, `E`) ≈ **2× body** | `B` + `S` ≈ **body + max string** | ≤ `max_body_bytes` |
| RSS | same (all heap) | same | same |

All of this memory is non-evictable heap.

### New approach — incremental tokenizer

Heap and RSS contributions are now independent, with behavior that differs between
Tier 1 and Tier 2 because of how `captureEnabled()` gates InCapture mode.

#### Tier 1 (body ≤ `max_element_capture_bytes`, default 256 KB)

`captureEnabled()` returns `true`. When `messages[i]` or `tools[i]` opens at
depth 3, `startCapture(*elem_writer_)` is called and the tokenizer enters
`ParseState::InCapture`.

In InCapture mode, `processByte()` skips all token-building paths entirely:
`token_buf_` is not touched. Every byte goes directly to
`elem_writer_->append()`, which writes into the mmap arena. No string is
built for any value inside the captured element, regardless of its size or
content.

```
heap contributors (Tier 1):
  token_buf_       — holds the current scalar outside capture (model name, etc.)
                     max = longest depth-1 string field, typically < 256 bytes
  handler members  — model_, messages_ vector of PayloadRef (12 B each), sampling_
                     < 1 KB total
  writer structs   — residual_writer_ + elem_writer_, ~32 bytes each on heap

RSS contributors (Tier 1):
  residual mmap    — full body streamed via residual_writer_           = B
  element mmap     — each captured element streamed via elem_writer_   = Σ E_i
  total            — B + Σ E_i  (evictable page-cache)
```

**Peak heap Tier 1: < 2 KB, O(1) with respect to body or element size.**

#### Tier 2 (body > `max_element_capture_bytes`, ≤ `max_body_bytes`)

`captureEnabled()` returns `false`. When `messages[i]` opens at depth 3,
`startCapture()` is **not** called. The tokenizer stays in normal mode and
continues tokenizing content inside the element at all depths. This means
`token_buf_` accumulates the full content of every string value encountered
inside the element, including large ones like base64 images.

This is the same per-string heap cost as the nlohmann transient — the mechanism
differs (`token_buf_` vs nlohmann's internal string) but the heap scaling is
identical: proportional to the largest string value in the body. The benefit in
Tier 2 is that `body_buffer_` is eliminated — the full body is in evictable mmap
page-cache rather than on heap.

```
heap contributors (Tier 2):
  token_buf_       — grows to the largest string value in the body (anywhere in doc)
                     max = S (may be large for multimodal)
  handler members  — < 1 KB

RSS contributors (Tier 2):
  residual mmap    — full body streamed via residual_writer_    = B
  no element mmap  — captureEnabled() false, no elem_writer_
```

**Peak heap Tier 2: ≈ `S` (largest string value) — body moved off heap, but per-string cost remains.**

| | Tier 1 (≤256 KB body) | Tier 2 (≤4 MB body) | Tier 3 (reject) |
|---|---|---|---|
| Heap | < 2 KB (O(1)) | ≈ `S` (largest string value in body) | ≤ one chunk |
| RSS (mmap) | `B` + Σ elements | `B` only | 0 |
| RSS total | `B` + Σ elements | `B` + `S` | ≤ one chunk |
| Heap OS-evictable | — | — | — |
| RSS OS-evictable | yes (all page-cache) | yes (mmap portion) | n/a |

### Multimodal worst-case walkthrough

**Scenario**: single message carrying a 10 MB raw image, base64-encoded to ~13.3 MB.
Total body ≈ 13.4 MB. Assume `max_body_bytes` is configured above this threshold;
`max_element_capture_bytes` = 256 KB (default).

Because body (13.4 MB) > `max_element_capture_bytes` (256 KB), this is **Tier 2**.
With default `max_body_bytes` = 4 MB the request would be Tier 3 (rejected).

```
Old approach, Tier 2:

  onData() chunks accumulated → body_buffer_ = 13.4 MB heap
  finish() → sax_parse()
    at base64 string (depth ~6):
      nlohmann allocates 13.3 MB std::string transient         heap: 13.4 + 13.3 = 26.7 MB ← peak
      string() callback: handler returns true (captureEnabled=false)
      transient freed                                           heap: 13.4 MB
    element close: no sliceBuffer (Tier 2)
  body_buffer_ freed after finish()

  Peak heap: 26.7 MB (body + transient)
  Peak RSS:  26.7 MB (all non-evictable heap)

New approach, Tier 2:

  onData() chunks:
    residual_writer_->append(chunk) → mmap arena              RSS:  grows to 13.4 MB (page-cache)
    tokenizer_.feed(chunk):
      captureEnabled()=false, no startCapture() at element open
      tokenizer stays in normal mode inside messages[0]
      at base64 string: token_buf_ accumulates 13.3 MB         heap: grows to 13.3 MB ← peak
      onString(token_buf_): handler ignores (depth > 1, Tier 2)
      token_buf_.clear()  — capacity retained at 13.3 MB
  finish() → finalize residual_writer_ → PayloadRef::External

  Peak heap: 13.3 MB (token_buf_ only — body is in mmap, not heap)
  Peak RSS:  13.4 MB (mmap, evictable) + 13.3 MB (token_buf_, heap) = 26.7 MB
```

Summary for this scenario:

| | Old (nlohmann, Tier 2) | New (tokenizer, Tier 2) |
|---|---|---|
| Body on heap | 13.4 MB | 0 (in mmap page-cache) |
| Large-string transient on heap | 13.3 MB (nlohmann std::string) | 13.3 MB (token_buf_) |
| Peak heap | **26.7 MB** | **13.3 MB** |
| Peak RSS | 26.7 MB (non-evictable) | 26.7 MB (13.4 MB evictable + 13.3 MB heap) |

The body is removed from heap (saving `B` = 13.4 MB of non-evictable heap), but
the large-string heap cost is not eliminated in Tier 2 — it is replaced by
`token_buf_` growth. Peak heap is halved; peak RSS is unchanged.

### Side-by-side comparison

| | Old (nlohmann SAX) | New (tokenizer, Tier 1) | New (tokenizer, Tier 2) |
|---|---|---|---|
| Body on heap | yes (`body_buffer_`) | no — mmap | no — mmap |
| Large-string heap transient | yes (nlohmann eager lexer) | **no** — InCapture skips tokenization | yes (`token_buf_` grows) |
| Per-element heap copy | yes (`sliceBuffer` string) | no — bytes go to mmap | n/a (no capture) |
| Peak heap | `B` + max(`S`,`E`) ≈ 2× body | < 2 KB | ≈ `S` |
| Peak RSS | same (all non-evictable heap) | `B` + Σ elements (evictable) | `B` + `S` (B evictable) |
| Large-string problem eliminated | — | **yes**, via InCapture mode | no, `token_buf_` has same scaling |

The large-string transient is fully eliminated only in **Tier 1**, where
`captureEnabled()` is true and InCapture mode is active. In Tier 2 the body
moves off heap (a meaningful improvement), but per-string heap scaling remains.

---

## Components

### `PayloadRef` — lightweight field handle

```
Storage::Inline    — field bytes held in a std::string inside the ref (≤ threshold)
Storage::Buffered  — field held in a heap Buffer::OwnedImpl (legacy path)
Storage::External  — field stored in the backing file of a MmapPayloadStore;
                     only {offset, length} are stored in the ref itself
```

`PayloadRef` is cheap to copy and store in vectors. `External` refs hold 12 bytes
(a `uint64_t` offset and a `size_t` length); the actual bytes live in the mmap
region of the associated store.

**Important**: `PayloadRef::toString()` PANICs on `External` refs. Callers that
may encounter external refs must use `convertPayloadRefToString(ref, request)` (see below).

### `StreamWriter` — incremental store interface

```cpp
class StreamWriter {
public:
  virtual ~StreamWriter() = default;
  virtual void       append(absl::string_view bytes) = 0;
  virtual PayloadRef finalize()                      = 0;
};
```

A `StreamWriter` session is opened via `PayloadStore::beginStore(kind)`. Callers
call `append()` any number of times to stream bytes incrementally, then call
`finalize()` once to commit and receive a `PayloadRef`. The implementation decides
storage placement (Inline, Buffered, or External) at finalize time based on the
total bytes written.

### `PayloadStore` — storage backend interface

```cpp
class PayloadStore {
  virtual PayloadRef store(std::string data,           PayloadKind kind) = 0;
  virtual PayloadRef store(Buffer::Instance& data,     PayloadKind kind) = 0;
  virtual std::unique_ptr<StreamWriter> beginStore(    PayloadKind kind) = 0;
  virtual void fetch     (const PayloadRef& ref,       FetchCallback cb) = 0;
  virtual void fetchAsync(const PayloadRef& ref,
                          Event::Dispatcher& dispatcher,
                          FetchCallback cb)                              = 0;
};
```

Two implementations ship:

| Class | Storage | When to use |
|---|---|---|
| `InMemoryPayloadStore` | heap (`std::string` / `Buffer::OwnedImpl`) | testing, low-memory-pressure environments |
| `MmapPayloadStore` | anonymous temp file via `mmap` | production; default in the filter |

### `MmapPayloadStore` — mmap-backed arena

**Backing file**: created with `mkstemp` in a configurable temp directory
(default `/tmp`), then immediately `unlink`ed. The file has no filesystem name;
it is accessible only through the open file descriptor and is reclaimed
automatically when the store is destroyed or the process exits.

**Layout**: bump-allocated arena. Each large payload is appended sequentially;
`PayloadRef::External` records the `{start_offset, length}` byte range.

```
  ┌─────────────────────────────────────────────────────┐
  │  payload A (4500 B) │ payload B (12000 B) │ ...     │
  └─────────────────────────────────────────────────────┘
  ↑                     ↑
  offset=0              offset=4500
```

**Capacity growth**: starts at 64 KB, doubles on overflow.
- Linux: `ftruncate` + `mremap(MREMAP_MAYMOVE)` — extends in place when possible.
- macOS: `ftruncate` + `munmap` + fresh `mmap` at the new size.

**Zero-copy `Buffer::Instance` ingestion**: `store(Buffer::Instance&)` walks
`getRawSlices()` and `memcpy`s each iovec slice directly into the mmap region,
avoiding a contiguous intermediate allocation.

**Fallback**: if `mkstemp` fails (e.g. read-only tmpfs) or any `ftruncate`/`mmap`
call fails, the store transparently falls back to `PayloadRef::Buffered` for that
payload. No error is surfaced to callers; all store/fetch operations remain valid.

**Thread safety**: not thread-safe. One store per request stream matches Envoy's
single-threaded filter chain model.

### `MmapStreamWriter` — incremental mmap writer

`MmapStreamWriter` is the `StreamWriter` implementation returned by
`MmapPayloadStore::beginStore()`.

```cpp
class MmapStreamWriter : public StreamWriter {
  MmapPayloadStore& store_;
  size_t            start_offset_;   // write_offset_ at construction time
  size_t            total_written_{0};
  bool              failed_{false};
};
```

- **Construction**: records `start_offset_ = store.write_offset_` — the arena
  position where this stream's bytes will begin.
- **`append(bytes)`**: calls `store_.ensureSpace(bytes.size())` and
  `store_.appendBytes()` directly, advancing `write_offset_` and incrementing
  `total_written_`. On any `ensureSpace` failure sets `failed_ = true` and
  silently drops subsequent bytes.
- **`finalize()`**: if `failed_`, falls back to a heap `Buffer::OwnedImpl`; if
  `total_written_ <= max_inline_bytes_`, copies back from the mmap region as
  `Inline`; otherwise returns `External{start_offset_, total_written_}`.

Two `MmapStreamWriter` sessions are active simultaneously during element capture:
`residual_writer_` (always open from construction, capturing the full body) and
`elem_writer_` (open during InCapture, capturing one element at a time). Both
write into the same arena; their byte ranges do not overlap because `write_offset_`
advances monotonically.

---

## `convertPayloadRefToString` — safe cross-storage accessor

```cpp
// ai_request.h / ai_request.cc
std::string convertPayloadRefToString(const PayloadRef& ref, const AiRequest& request);
```

Routes through the correct path for every storage variant:

- `Inline` / `Buffered` → calls `ref.toString()` directly.
- `External` → calls `request.payload_store->fetch(ref, ...)` to read from the
  mmap region.

All encoder call sites (`request_encoder.cc`, `anthropic_request_encoder.cc`) use
`convertPayloadRefToString` rather than `ref.toString()` so they remain correct regardless of
which store the filter is using.

---

## Async External Payload Fetch

### Problem

`MmapPayloadStore` stores large payloads in an mmap-backed temp file. When an
encoder reads them back via `fetch()`, it calls `memcpy` directly from the mmap
region on the Envoy event loop thread. On the first access after a page eviction,
that `memcpy` blocks on a page fault — the kernel must re-read the page from the
temp file on disk. This is real I/O that can take milliseconds and stalls the
entire worker thread.

### Solution: three-layer async pipeline

**Layer 1 — `MmapPayloadStore::fetchAsync`**

Spawns a short-lived detached thread that calls `pread()` (rather than direct
mmap access) so any page fault blocks only that thread, not the event loop. The
fd is captured as a plain `int` by value, so the read is safe even if the store
is destroyed before the thread completes. When done, `dispatcher.post()` marshals
the result buffer back to the event loop thread.

```
event loop thread              detached thread
      │                              │
      │── fetchAsync() ─────────────▶│  pread()  [may page-fault here]
      │   (returns immediately)      │
      │                              │── dispatcher.post(callback)
      │◀─────────────────────────────│
      │  callback fires with Buffer
```

**Layer 2 — `prefetchExternalPayloadRefs` (`ai_request.cc`)**

Walks all `PayloadRef` fields in `AiRequest` (messages, tools, attachments,
parts, arguments, capabilities, params_raw, residual_params) and collects every
`External` ref. Fans out a `fetchAsync` call for each, using a
`shared_ptr<atomic<size_t>>` countdown. When the counter reaches zero, `on_done`
is called exactly once, on the event loop thread. Each completed fetch upgrades
the ref in-place from `External` to `Buffered`, so all downstream encoders can
call `ref.toString()` without knowing the store type.

**Layer 3 — wired into `dispatch()` (`filter.cc`)**

`dispatch()` already posted `doDispatch()` to the next event loop tick to satisfy
Envoy's re-entrancy rules. The post now calls `prefetchExternalPayloadRefs` first;
`doDispatch()` runs only after all External refs have been materialized:

```
dispatch()
  └─ dispatcher.post()              ← re-entrancy guard (pre-existing)
       └─ prefetchExternalPayloadRefs()
            ├─ no External refs  →  doDispatch() immediately
            └─ N External refs   →  fan out fetchAsync × N
                                     └─ on_done: doDispatch()
```

### Key design properties

- **Non-blocking**: page faults happen on a detached thread, never on the worker.
- **Exactly-once `on_done`**: the atomic countdown guarantees `doDispatch()` fires
  once regardless of how many External refs exist.
- **Encoder-transparent**: after prefetch all refs are `Buffered`; no encoder
  call site needed to change.
- **Safe across lifetimes**: the fd is captured by value; if the store is
  destroyed mid-flight `pread()` returns -1 and the callback still fires with an
  empty buffer rather than crashing.
- **`InMemoryPayloadStore` unaffected**: its refs are never `External`, so
  `prefetchExternalPayloadRefs` returns via the `external.empty()` fast path immediately.

### Why writes are synchronous

`store()` and `StreamWriter::append()` run on the event loop thread and write via
`memcpy` into the mmap region during `onData()`, as each HTTP chunk arrives.
Writes to newly-allocated pages cause write page faults, but these are cheaper
than read page faults:

- **Read fault on an evicted page**: the kernel must re-read from disk — actual
  I/O, potentially milliseconds.
- **Write fault on a new page**: the kernel zero-initializes a fresh physical
  page and maps it in — no disk I/O, just memory allocation.

The just-`ftruncate`'d pages are brand-new; the OS has had no time to evict them.
The eviction-under-memory-pressure risk that motivates async reads does not apply.

Making writes async would require pre-reserving arena offsets, staging body bytes
in a secondary buffer while the async copy races ahead, and stalling or buffering
tokenizer output — adding significant complexity for no practical benefit.

### External Cache Consideration

If the backing store were an external cache (Redis, memcached, any remote store)
rather than a local mmap file, both the write and read paths would need async
treatment — and the write path would additionally require a secondary buffer.

**Why writes need a secondary buffer**

With mmap, `StreamWriter::append()` is synchronous: `memcpy` into the mapped
region completes before the function returns, so a valid `PayloadRef::External`
can be handed back immediately. The arena offset is stable the moment the write
finishes.

With an external cache the write is a network round-trip. The event loop cannot
block waiting for it, so `append()` cannot complete synchronously. The data must
live somewhere while the async write is in flight — that is the secondary buffer:

```
append() called by tokenizer InCapture mode
  │
  ├─ copy data into heap Buffer::OwnedImpl   ← secondary (temporary) buffer
  ├─ return immediately                      ← tokenizer continues
  └─ fire async write to external cache
        │
        └─ on write confirm: upgrade ref Buffered → External{cache_key}
```

**Comparison: mmap vs external cache**

| | mmap | external cache |
|---|---|---|
| Write | synchronous `memcpy`, no extra allocation | async network write + heap copy as staging buffer |
| Read | async only when page evicted (uncommon) | async on every read (every access is a network round-trip) |
| Memory | pages evictable by OS; physical RAM reclaimed under pressure | heap copy lives until write confirms; cache holds a second copy |

mmap threads the needle: writes are fast enough to be synchronous (no secondary
buffer needed), the OS handles eviction transparently, and reads are only async
when memory pressure actually forces a page out.

---

## Full Request Workflow

This section traces a complete `POST /v1/chat/completions` request from arrival
to dispatch, showing exactly what happens at each stage with the incremental
tokenizer and streaming store.

### Phase 1 — Headers (`decodeHeaders` → `onHeaders`)

```
decodeHeaders(headers, end_stream=false)
  └─ RequestDecoder::onHeaders(headers)
       ├─ classify(method="POST", path="/v1/chat/completions")
       │    → ProtocolKind::OpenAiInference
       ├─ state_ = ParsingInferenceBody
       └─ InferenceBodyParser constructed:
            ├─ residual_writer_ = store_.beginStore(PayloadKind::ResidualParams)
            │    → MmapStreamWriter{ start_offset_=0, total_written_=0 }
            └─ IncrementalJsonTokenizer initialized, InferenceHandler attached
```

The `MmapStreamWriter` for `residual_params` is opened immediately. No bytes have
arrived yet but the arena offset is reserved.

### Phase 2 — Body chunks (`decodeData` → `onData`, called 1..N times)

For each HTTP data frame:

```
decodeData(chunk, end_stream=false)
  └─ RequestDecoder::onData(chunk)
       └─ InferenceBodyParser::feed(chunk)
            ├─ check: total_bytes_ + chunk.size() ≤ max_body_bytes  [Tier 3 guard]
            ├─ total_bytes_ += chunk.size()
            ├─ residual_writer_->append(chunk)          ← streams to mmap arena
            └─ tokenizer_.feed(chunk)
                 └─ processByte(c, reprocess) for each byte c in chunk:
                      ├─ [depth=1, key="model"]  → InferenceHandler::onKey("model")
                      ├─ [depth=1, string value] → InferenceHandler::onString("gpt-4o")
                      │    → handler_.model_ = "gpt-4o"
                      ├─ [depth=1, key="stream"] → ... onBool(true)
                      │    → handler_.stream_ = true
                      ├─ [depth=2, key="messages"] → handler_.in_messages_ = true
                      ├─ [depth=3, onStartObject]  → [if captureEnabled()]
                      │    elem_writer_ = store_.beginStore(MessageElement)
                      │    tokenizer_.startCapture(*elem_writer_)
                      │    state_ = InCapture
                      ├─ [InCapture bytes] → elem_writer_->append({&c, 1})
                      │    → MmapStreamWriter::append → ensureSpace + appendBytes
                      └─ [InCapture end: cap_depth_counter_==0 on `}`]
                           elem_writer_->finalize() → PayloadRef::External{off, len}
                           handler_.messages_.push_back(ref)
                           state_ = ExpectCommaOrClose
```

After each `onData()` returns:
- `residual_writer_` holds all bytes received so far, streamed incrementally to the
  mmap arena.
- Any completed `messages[]` / `tools[]` elements are in `handler_.messages_` /
  `handler_.tools_` as `PayloadRef::External` handles.
- No full-body buffer exists; peak memory is one HTTP chunk plus the active mmap
  writer regions.

### Phase 3 — End of stream (`decodeTrailers` or `end_stream` flag → `onEndStream`)

```
decodeData(last_chunk, end_stream=true)   [or decodeTrailers()]
  └─ RequestDecoder::onData(last_chunk)   [same as Phase 2]
  └─ RequestDecoder::onEndStream()
       └─ InferenceBodyParser::finish()
            ├─ tokenizer_.finish()             ← validates document is complete
            ├─ residual_writer_->finalize()
            │    → PayloadRef for full body (External if > max_inline_bytes)
            ├─ Move handler_ fields into InferencePayload:
            │    payload_.model        = std::move(handler_.model_)
            │    payload_.messages     = std::move(handler_.messages_)
            │    payload_.tools        = std::move(handler_.tools_)
            │    payload_.sampling     = handler_.sampling_
            │    payload_.stream       = handler_.stream_
            │    payload_.residual     = residual_ref
            └─ Move InferencePayload into AiRequest
       └─ state_ = BodyComplete
```

Auth ordering is satisfied: all fields accumulated during Phase 2 in `handler_`
members are only committed to the real `AiRequest` payload here, inside
`onEndStream()`, after any body-covering authentication upstream has completed.

### Phase 4 — Dispatch

```
RequestDecoder::take()
  └─ returns std::move(request_)   [AiRequest now fully populated]
  └─ state_ = AwaitingHeaders      [decoder reset for potential reuse]

filter.cc::dispatch()
  └─ dispatcher.post()              ← re-entrancy guard
       └─ prefetchExternalPayloadRefs(request, dispatcher, on_done)
            ├─ collect all External PayloadRefs from messages, tools,
            │  attachments, params_raw, residual_params, ...
            ├─ if none: doDispatch() immediately
            └─ if N > 0: for each ref:
                 store.fetchAsync(ref, dispatcher, [ref, on_done](...) {
                   ref.upgrade(Buffered)
                   if (--countdown == 0) doDispatch()
                 })
                 (pread() on detached thread; dispatcher.post back to event loop)
```

### Phase 5 — Encode

```
doDispatch()
  └─ sub_chain_.run(request)
       └─ RequestEncoder / AnthropicRequestEncoder
            └─ for each messages[i]:
                 convertPayloadRefToString(ref, request)
                   → ref is now Buffered (upgraded by prefetch)
                   → ref.toString()  [no store access needed]
```

All `External` refs have been upgraded to `Buffered` by `prefetchExternalPayloadRefs`
before `doDispatch()` runs. Encoders call `ref.toString()` with no mmap access.

---

## Configuration

The filter uses `MmapPayloadStore` by default, constructed with:

```cpp
MmapPayloadStore payload_store_{"/tmp", config->decoderConfig().max_inline_bytes};
```

`DecoderConfig` exposes three thresholds:

| Field | Default | Effect |
|---|---|---|
| `max_inline_bytes` | 4096 B | PayloadStore inline/offload boundary. Fields at or below this size are stored `Inline` (in process memory); larger fields go to the mmap backing file as `External` refs. |
| `max_element_capture_bytes` | 256 KB | Soft body-size limit. Bodies at or below this size get full per-element streaming capture (`messages[]`, `tools[]`, `params`). Bodies above it extract scalars only. Checked per-chunk via `captureEnabled()`. |
| `max_body_bytes` | 4 MB | Hard body-size limit. `feed()` returns `ResourceExhausted` as soon as a chunk would push the running total past this ceiling. |

---

## Tests

| Test file | What it covers |
|---|---|
| `test/.../payload_store_test.cc` | 23 unit tests: `PayloadRef::External` accessors, inline threshold boundary, fetch roundtrip for strings and multi-slice buffers, capacity doubling (forces `mremap`/`munmap+mmap` path), `MmapStreamWriter` incremental append and finalize, `InMemoryPayloadStore` regression |
| `test/.../request_decoder_test.cc` | 6 unit tests: body-size tiering for both `InferenceBodyParser` and `AgentBodyParser` — Tier 1 (elements/params captured via StreamWriter), Tier 2 (scalars only, no element capture), Tier 3 (hard limit, `ResourceExhausted` from `onData`) |
| `mcp_auth_filter_integration_test.cc` | 5 integration tests: `makeLargeToolsCallBody` pads JSON-RPC bodies past 4096 B so the tokenizer stores `messages[]` elements as `External` refs; verifies auth, routing, and tool-name extraction work end-to-end |
| `mcp_auth_rest_integration_test.cc` | 3 integration tests: large payload REST transcoding through `AgentBodyParser`, unknown-tool fallback, auth-before-transcoding ordering |

---

## Invariants

1. `PayloadRef::toString()` **must not** be called on `External` refs — it will
   PANIC. Use `convertPayloadRefToString(ref, request)` at every encoder call site.
2. The `MmapPayloadStore` instance must outlive all `External` `PayloadRef`s
   created from it. The filter satisfies this: the store is a filter member and
   refs are owned by `AiRequest`, which is destroyed before the filter.
3. A store that failed to mmap still returns valid refs (falling back to
   `Buffered`). Callers never need to check store health.
4. `StreamWriter::finalize()` must be called exactly once per session. Calling
   `append()` after `finalize()` is undefined behavior.
5. Auth ordering: no parsed field from `InferenceHandler` or `AgentHandler` is
   moved into the `AiRequest` until `finish()` is called from `onEndStream()`.
   `feed()` accumulates into handler members only.

---

## End-to-end flow — incremental parsing with both storage backends

This section traces a single request from the first byte in `decodeHeaders` to
the upstream write, showing exactly where each storage backend diverges.

### Setup

The filter constructs one `PayloadStore` for the lifetime of the request stream.
Which implementation is used determines how large fields are backed:

| Context | Implementation | Large field backing |
|---|---|---|
| Production filter | `MmapPayloadStore("/tmp", 4096)` | mmap-backed temp file (OS page-cache) |
| Unit / integration tests | `InMemoryPayloadStore(4096)` | heap `Buffer::OwnedImpl` |

Both satisfy the same `PayloadStore` interface. The parser and dispatch layers
never inspect which backend is active.

If `MmapPayloadStore` fails to create or map its backing file (e.g. no disk space),
it transparently degrades: large fields fall back to `PayloadRef::Buffered` (heap)
rather than `PayloadRef::External`, so the system stays functional.

---

### Phase 1 — Headers (`RequestDecoder::onHeaders`)

The classifier inspects HTTP method, path, and headers. No body bytes have
arrived. Based on the result it allocates the right body parser and sets decoder
state:

- Inference path → `InferenceBodyParser` created, state = `ParsingInferenceBody`
- Agent path (MCP / A2A) → `AgentBodyParser` created, state = `ParsingAgentBody`

No `PayloadStore` calls happen here.

---

### Phase 2 — Body chunks (`RequestDecoder::onData` → `parser.feed()`)

Each chunk runs two paths in parallel before returning.

#### Path A — Raw body streaming (full-body capture)

On the first chunk the parser opens a streaming write session:

```cpp
residual_writer_ = store_.beginStore(PayloadKind::JsonObject);
```

Every subsequent chunk is appended verbatim before the tokenizer sees it:

```cpp
residual_writer_->append(chunk);
```

What this produces depends on the backend:

| Backend | `beginStore` returns | `finalize()` returns |
|---|---|---|
| `MmapPayloadStore` | `MmapStreamWriter` — appends into the mmap arena at `write_offset_`, advances the cursor | `PayloadRef::External{start_offset, total}` for large; `PayloadRef::Inline` for small |
| `InMemoryPayloadStore` | Writer accumulating into a heap `std::string` | `PayloadRef::Buffered(OwnedImpl)` for large; `PayloadRef::Inline` for small |

The full raw body lands in storage incrementally, with no contiguous intermediate
heap buffer.

#### Path B — Incremental semantic parsing

The same chunk bytes are fed to `IncrementalJsonTokenizer`. The 14-state machine
processes one byte at a time through explicit data-member state. There is no
recursion and no call-stack parse frame — parsing suspends at any byte and resumes
on the next `feed()` call.

The tokenizer operates in two modes:

**Semantic mode** fires `Handler` callbacks as tokens complete. The handler
extracts scalar fields (`model`, `stream`, `method`, `id`, sampling params, etc.)
directly into plain struct members. The only allocation is `token_buf_`, which
accumulates the bytes of whichever string or number token is currently in flight.

**Capture mode** (`ParseState::InCapture`) activates when the handler calls
`startCapture(writer)` from inside an `onStartObject` or `onStartArray` callback.
The tokenizer stops firing semantic events and instead forwards every raw byte
directly into the `StreamWriter`. It tracks only quote, escape, and nesting depth
to detect the container's closing delimiter. No `token_buf_` growth, no per-byte
heap allocation. When the matching `}` or `]` arrives, `onEndObject` /
`onEndArray` fires once and the tokenizer returns to semantic mode.

**Where capture activates:**

| Parser | Trigger | `StreamWriter` opened on |
|---|---|---|
| `InferenceBodyParser` | Object or array at depth 3 inside `messages[]` or `tools[]` | `store_.beginStore()` — same backend as `residual_writer_` |
| `AgentBodyParser` | `params` value at depth 2 | `StringStreamWriter` (inline heap `std::string`) |

`AgentBodyParser` uses a `StringStreamWriter` (not the `PayloadStore`) for params
because the captured bytes must be passed to `nlohmann::json::parse()` in
`finish()` to extract sub-fields like `tool_name`. The heap cost is bounded by
the size of the params object, not the full body.

**Concurrent writer sessions during element capture (`MmapPayloadStore` only):**

While an `InferenceBodyParser` element is being captured, two `MmapStreamWriter`
sessions are active simultaneously — `residual_writer_` writing the full body and
`elem_writer_` writing the captured element. Both advance the same `write_offset_`
into the same mmap arena from different starting offsets. The arena's bump-allocator
layout ensures no overlap:

```
mmap arena
  [0 .................. residual_start]
                          ↑
                    residual_writer_ (full body, always open)
                          
  [....... elem_start .... elem_end ..]
                ↑
          elem_writer_ (open during InCapture only)
```

#### Duplicate-key detection (inline with semantic parsing)

Inside `onKey()`, before any field extraction, the handler checks a `seen_*` bool
for each known depth-1 key. On a second occurrence it sets `has_error_` with the
specific key name and returns `false`. The tokenizer converts this into
`InvalidArgument`. `feed()` surfaces the handler's error string. Back in
`filter.cc::decodeData()`, `!status.ok()` triggers `sendLocalReply(400)` —
the request is rejected before auth, routing, or the upstream sees it.

---

### Phase 3 — End of stream (`RequestDecoder::onEndStream` → `parser.finish()`)

1. `tokenizer_.finish()` — flushes any trailing number token (numbers have no
   terminator character; the end of input is their close signal).
2. `residual_writer_->finalize()` — commits the full raw body and returns a
   `PayloadRef` stored as `payload.residual_params`.
3. **`AgentBodyParser` only** — passes the captured `params_buf_` to
   `nlohmann::json::parse()`. Only the already-isolated params bytes are re-parsed,
   not the full body. Extracted sub-fields (`tool_name`, `resource_uri`,
   `prompt_name`) are stored via `store_.store()`.
4. All accumulated scalars and `PayloadRef`s are moved into the `AiRequest` payload.
   Nothing is copied: `std::move` transfers ownership of strings and refs.

---

### Phase 4 — Chain and dispatch

The `AgenticChain` (including `McpAuthFilter`) runs against the `AiRequest`. Auth
and routing decisions use plain strings (`rpc_method`, `tool_name`, `principal`) —
no `PayloadStore` access is needed at this phase.

When `AgenticDispatch` re-encodes the body to forward upstream, it materialises
`External` refs back into bytes:

```cpp
PayloadStore::fetchAsync(ref, dispatcher, callback);
```

| Backend | `External` ref behaviour |
|---|---|
| `MmapPayloadStore` | Spawns a detached thread calling `pread(fd, offset, len)`. Page faults happen off the event loop thread. Callback is posted back to the dispatcher once the read completes. |
| `InMemoryPayloadStore` | No `External` refs are ever produced. `fetch()` resolves `Inline` and `Buffered` refs synchronously via `ref.toString()`. |

`Inline` and `Buffered` refs are always resolved synchronously on both backends.
`prefetchExternalPayloadRefs` upgrades all `External` refs to `Buffered` before
`doDispatch()` runs, so encoders call `ref.toString()` with no mmap access.

---

### `PayloadRef` storage variants — where each backend produces them

```
PayloadRef
 ├── Inline   ≤ max_inline_bytes
 │            both backends
 │            data in std::string, on heap, synchronous fetch
 │
 ├── Buffered > max_inline_bytes
 │            InMemoryPayloadStore (always)
 │            MmapPayloadStore (fallback when mmap unavailable)
 │            data in Buffer::OwnedImpl, on heap, synchronous fetch
 │
 └── External > max_inline_bytes
              MmapPayloadStore only (when mmap healthy)
              data in OS page-cache, not heap malloc
              {offset, length} into the mmap arena
              fetched via pread() on a background thread
```

---

### Peak heap by tier and backend

| Tier | Condition | `MmapPayloadStore` heap | `InMemoryPayloadStore` heap |
|---|---|---|---|
| 1 | Body ≤ `max_element_capture_bytes` (256 KB) | One chunk window + `MmapStreamWriter` metadata | One chunk window + all element strings accumulating in heap writers |
| 2 | Body ≤ `max_body_bytes` (4 MB) | One chunk window + `token_buf_` (scales with largest string value) | Full body in `OwnedImpl` + `token_buf_` |
| 3 | Body > `max_body_bytes` | Rejected in `feed()` before `finalize()` is reached | Same |

The mmap arena is RSS (page-cache backed). It does not count against the process's
malloc budget, and the kernel can evict pages under memory pressure as long as the
file descriptor remains open.

---

## Deep Memory Analysis

### Taxonomy: heap vs RSS

Before counting allocations it is important to distinguish two memory concepts:

**Heap** — memory returned by `malloc` / `new`. Non-evictable. Counted by RSS and
by the allocator's own bookkeeping. A spike here directly competes with every other
allocation in the process.

**RSS (Resident Set Size)** — all physical pages currently mapped into the process,
including heap, stack, code segments, and mmap regions. mmap pages are included in
RSS but are backed by the page-cache: the kernel can silently evict them under
memory pressure and reload them from the file descriptor on next access, without
the process noticing. A spike in RSS from mmap does not starve the heap allocator.

All figures below track **heap only** unless explicitly noted as RSS.

---

### The nlohmann eager-lexer problem (old design)

Under the old design the full body was buffered first, then parsed with nlohmann's
`sax_parse()`. Even in SAX mode, nlohmann's internal lexer is eager: it fully
tokenizes each JSON value — allocating a `std::string` for every string it
encounters — before firing any callback. The handler cannot intercept this.

For a multimodal inference request with a 10 MB base64 image:

```json
{
  "model": "gpt-4o",
  "messages": [{"role": "user", "content": [{"type": "image_url",
    "image_url": {"url": "data:image/jpeg;base64,<10 MB>"}}]}]
}
```

The old peak heap had two simultaneous allocations:

```
body_buffer_  — body buffered before parse          ≈ 10 MB   (heap)
nlohmann transient — std::string for the image URL  ≈ 10 MB   (heap, inside sax_parse)
─────────────────────────────────────────────────────────────
peak heap                                           ≈ 20 MB   for a 10 MB body
```

These two exist at the same time: `sax_parse()` runs against the already-buffered
body, so `body_buffer_` has not been freed yet when the transient is allocated.
After `sax_parse()` returns the transient is freed, and shortly after `body_buffer_`
is freed too — but their simultaneous peak is approximately `2 × body`.

---

### New design: how the tokenizer eliminates the transient

`IncrementalJsonTokenizer` never builds a string for content that is being captured.
When the handler calls `startCapture(writer)` on the opening `{` of a messages
element, the tokenizer enters `ParseState::InCapture`. From that point every byte is
forwarded directly to the `StreamWriter` — the `InCapture` case in `processByte()`
does not touch `token_buf_` at all:

```cpp
case ParseState::InCapture:
    capture_writer_->append({reinterpret_cast<const char*>(&c), 1});
    // track quote/escape/depth only — no token_buf_ involvement
```

So for the same 10 MB image, the tokenizer never enters `InStringValue` state for
the image string. The `"data:image/jpeg;base64,<10 MB>"` bytes pass through
`InCapture` one at a time, each going straight to the `StreamWriter`. No
`std::string` of any size representing the image is ever allocated on the heap.

The transient allocation problem is eliminated entirely, regardless of which
`PayloadStore` backend is in use.

---

### `InMemoryStreamWriter` — heap behaviour in capture mode

`InMemoryStreamWriter` is the `StreamWriter` returned by `InMemoryPayloadStore::beginStore()`.
Its `append()` calls `buf_.add()` on a heap `Buffer::OwnedImpl`:

```cpp
void append(absl::string_view bytes) override {
    buf_.add(bytes.data(), bytes.size());   // grows heap OwnedImpl
}

PayloadRef finalize() override {
    return store_.store(buf_, kind_);       // → PayloadRef::Buffered
}
```

For the 10 MB image in capture mode with `InMemoryPayloadStore`:

```
InCapture byte loop → append() → buf_.add() → OwnedImpl grows to 10 MB on heap
finalize()          → PayloadRef::Buffered  → OwnedImpl transferred into ref
```

The 10 MB **does** end up on heap, but the critical difference from nlohmann is
that it is the **only** 10 MB allocation. There is no separate body buffer because
`residual_writer_` streams the full body into storage incrementally chunk-by-chunk
rather than buffering it all before parsing. The nlohmann `body_buffer_ + transient`
double-count is gone.

| Allocation | nlohmann (old) | `InMemoryStreamWriter` (new) |
|---|---|---|
| Body buffer before parse | ≈ 10 MB (mandatory) | None — streamed chunk by chunk |
| Tokenizer transient per string | ≈ 10 MB (mandatory, eager lexer) | None — `InCapture` bypasses `token_buf_` |
| Element storage after capture | ≈ 10 MB (copy into `PayloadRef::Buffered`) | ≈ 10 MB (`OwnedImpl` in `PayloadRef::Buffered`) |
| **Peak heap** | **≈ 20 MB** (body + transient simultaneous) | **≈ 10 MB** (element only) |

`InMemoryStreamWriter` solves the transient problem but the captured element still
lives on heap. This is acceptable for tests. Production uses `MmapStreamWriter`.

---

### `MmapStreamWriter` — heap behaviour in capture mode

`MmapStreamWriter::append()` calls `store_.appendBytes()`, which does a `memcpy`
into the mmap arena:

```cpp
void MmapStreamWriter::append(absl::string_view bytes) {
    store_.appendBytes(bytes.data(), bytes.size());  // memcpy into mmap region
    total_written_ += bytes.size();
}
```

The mmap region is page-cache backed — it is RSS but not heap. For the 10 MB image:

```
InCapture byte loop → append() → memcpy into mmap arena  (RSS, not heap)
finalize()          → PayloadRef::External{offset, len}  (8-byte struct on heap)
```

The only heap allocation for the entire 10 MB element is the 8-byte `PayloadRef`
struct that records where in the arena it lives. The element bytes themselves are
in the page-cache and can be evicted by the kernel at any time while the fd is open.

| Allocation | `MmapStreamWriter` |
|---|---|
| Body buffer before parse | None |
| Tokenizer transient per string | None |
| Element storage after capture | None on heap — bytes in mmap page-cache (RSS only) |
| `PayloadRef` handle | 8 bytes on heap |
| **Peak heap** | **One chunk window + handler state** |

---

### `token_buf_` — the remaining heap cost in semantic mode

`token_buf_` is the only field that grows on the heap during tokenization in
semantic mode. It accumulates the bytes of whichever string or number token is
currently in flight and is cleared after each callback fires.

Its peak size at any point equals the length of the **longest single string token**
the tokenizer has seen in semantic mode. In practice this means:

- Short scalar fields (`"model"`, `"stream"`, small string values) → `token_buf_`
  stays small (tens to hundreds of bytes)
- A long `"stop"` sequence string or a verbose `"model"` name → `token_buf_`
  grows to match, then is cleared

Crucially, `token_buf_` is **not** involved when the tokenizer is in `InCapture`
state. The image URL, the tool schema, the message content — none of those bytes
touch `token_buf_`. Only the envelope scalars at depth 1 that the handler extracts
(and which are intentionally small) pass through `token_buf_` in semantic mode.

In Tier 2 (body > `max_element_capture_bytes`, capture disabled), the tokenizer
stays in semantic mode throughout. Any large string value nested inside `messages`
or `tools` — a base64 image, a long tool description — will grow `token_buf_` to
match its length. This is the Tier 2 heap cost and it is unavoidable without
capture mode.

---

### Concurrent writer sessions — heap accounting

During element capture in `InferenceBodyParser`, two writers are active
simultaneously. Their heap cost depends on the backend:

```
residual_writer_  (open from first chunk, always)
elem_writer_      (open during InCapture only)
```

**`MmapPayloadStore`**: both writers are `MmapStreamWriter` instances. Each holds
only a `size_t start_offset_` and a `size_t total_written_` — a handful of bytes
on the heap. The bytes they write go into the mmap arena (RSS, not heap). Peak
heap from both writers combined is negligible.

**`InMemoryPayloadStore`**: both writers are `InMemoryStreamWriter` instances, each
holding a `Buffer::OwnedImpl buf_`. At the moment a messages element finishes
capturing, both `buf_`s are live simultaneously:

```
residual_writer_.buf_  — full body accumulated so far  ≈ body_so_far bytes (heap)
elem_writer_.buf_      — just the element              ≈ element bytes (heap)
```

For a 10 MB body where the image element spans most of it, both could be large at
the same time. This is the main reason `InMemoryPayloadStore` is not used in
production.

---

### Multimodal worst-case — full heap walkthrough

**Scenario**: 10 MB tools/call body, `params` contains a 9 MB base64 image
argument. Body exceeds `max_element_capture_bytes` (256 KB) → **Tier 2**.

Because capture is disabled in Tier 2, the `AgentBodyParser` tokenizer stays in
semantic mode throughout. When it reaches the 9 MB base64 string inside `params`,
it is in `InStringValue` state and accumulates every byte into `token_buf_`:

```
token_buf_  — 9 MB string value accumulating              9 MB  (heap)
```

Meanwhile `residual_writer_` is streaming the full body into the store. With
`MmapPayloadStore` that is in the mmap arena (RSS). With `InMemoryPayloadStore`
that is a growing `OwnedImpl` (heap).

Peak heap by backend:

```
MmapPayloadStore:
    token_buf_              ≈ 9 MB  (heap)
    MmapStreamWriter state  < 1 KB  (heap)
    mmap arena              ≈ 10 MB (RSS, not heap)
    ─────────────────────────────────────────
    peak heap               ≈ 9 MB

InMemoryPayloadStore:
    token_buf_              ≈ 9 MB  (heap)
    residual OwnedImpl      ≈ 10 MB (heap)
    ─────────────────────────────────────────
    peak heap               ≈ 19 MB
```

The `token_buf_` cost in Tier 2 with `MmapPayloadStore` is therefore comparable to
the old nlohmann transient — both scale with the largest string value. The
difference is that the body buffer (`body_buffer_` in the old design, `OwnedImpl`
in `InMemoryPayloadStore`) is eliminated.

**Scenario**: same 10 MB body, `max_element_capture_bytes` = 256 KB is respected →
body is ≤ threshold → **Tier 1**. Capture fires for `params`.

```
MmapPayloadStore:
    token_buf_              < 1 KB  (heap — only depth-1 scalars: method, id)
    MmapStreamWriter state  < 1 KB  (heap)
    mmap arena              ≈ 10 MB (RSS, not heap)
    ─────────────────────────────────────────
    peak heap               < 2 KB

InMemoryPayloadStore:
    token_buf_              < 1 KB  (heap)
    residual OwnedImpl      ≈ 10 MB (heap)
    params OwnedImpl        ≈ 10 MB (heap, concurrent with residual during capture)
    ─────────────────────────────────────────
    peak heap               ≈ 20 MB  (both OwnedImpl live simultaneously)
```

Tier 1 with `MmapPayloadStore` is where the design fully delivers: the entire 10 MB
body is in the page-cache and the process heap is essentially untouched regardless
of body size.

---

### Summary — heap cost per component

| Component | When active | `MmapPayloadStore` heap | `InMemoryPayloadStore` heap |
|---|---|---|---|
| `residual_writer_` session | Always, from first chunk | `MmapStreamWriter` metadata only (< 1 KB) | `OwnedImpl` growing to full body size |
| `elem_writer_` session | During `InCapture` (Tier 1 only) | `MmapStreamWriter` metadata only (< 1 KB) | `OwnedImpl` growing to element size |
| `token_buf_` | Semantic mode only | Largest depth-1 scalar (typically small) | Same |
| `token_buf_` in Tier 2 | Semantic mode throughout (no capture) | Largest string value anywhere in body | Same |
| `PayloadRef::External` handle | After `finalize()` | 8 bytes per ref | N/A (produces `Buffered` instead) |
| `PayloadRef::Buffered` | After `finalize()` | N/A (produces `External` instead) | Full element size in `OwnedImpl` |
| nlohmann transient (old design) | During `sax_parse()` | Eliminated | Eliminated |
| Body buffer before parse (old design) | Before `sax_parse()` | Eliminated | Eliminated |
