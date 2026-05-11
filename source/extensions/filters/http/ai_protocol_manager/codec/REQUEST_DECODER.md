# Request Decoder & Payload Store — AI Protocol Manager Codec

## Motivation

AI inference and agentic request bodies are structurally unlike typical REST
payloads. A single `POST /v1/chat/completions` may carry dozens of conversation
turns, each potentially containing base64-encoded images or large tool schemas,
pushing the JSON body into the hundreds of kilobytes. Under concurrent load the
naive approach — buffer the full body, parse it into a DOM, copy each field into a
`std::string` — creates two compounding problems:

**Problem 1 — full-body DOM parse**: `nlohmann::json::parse()` requires the entire
body to be contiguous in memory before it returns anything. For a 200 KB body
across thousands of concurrent streams this means hundreds of megabytes of heap
just to hold transient parse input, all of which becomes garbage the moment fields
are extracted.

**Problem 2 — field copies as heap strings**: Once parsed, each `messages[]`
element and `tools[]` definition is typically stored as a re-serialized
`std::string`. Multiple filters may then each hold their own copy, multiplying the
RSS impact and fragmenting the allocator under high concurrency.

Two complementary designs eliminate both problems:

1. **Streaming SAX parser** (`RequestDecoder`) — the body is never fully parsed
   into a DOM. `nlohmann::json::sax_parse()` reads directly from Envoy's
   `Buffer::OwnedImpl` slab chain via a zero-copy iterator. Scalar fields are
   extracted as SAX events fire; large sub-documents (`messages[]` elements,
   `tools[]` entries, `params`) are captured by recording their start/end byte
   positions in the buffer and copying the raw byte range directly — no DOM, no
   re-serialization, bytes identical to the original request body.

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
onHeaders()    — classify protocol, init body parser
onData()       — accumulate body chunks into a Buffer::OwnedImpl
onEndStream()  — run SAX parse, populate AiRequest, hand to chain
take()         — move completed AiRequest out of the decoder
```

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

### `BufferByteIterator` — zero-copy SAX input

SAX parsing reads directly from Envoy's `Buffer::OwnedImpl` slab chain without
copying body bytes into a contiguous `std::string` first.

`BufferByteIterator` is a C++ `InputIterator` that walks across a
`Buffer::RawSliceVector` (an iovec-style list of non-contiguous memory regions).
It is passed directly to `nlohmann::json::sax_parse()`:

```
Buffer::OwnedImpl (slab chain)
  ┌─────────┐  ┌─────────┐  ┌─────────┐
  │ slice 0 │  │ slice 1 │  │ slice 2 │
  └────┬────┘  └────┬────┘  └────┬────┘
       └─────────────┴─────────────┘
                     ↑
             BufferByteIterator
                     ↓
            nlohmann::sax_parse()
```

No intermediate allocation. The slab memory is read in place.

### `sliceBuffer` + byte-range capture — zero-normalization element extraction

For `messages[]` / `tools[]` elements (inference) and the `params` object
(agent), the parser uses **byte-range capture** rather than any form of
re-serialization:

1. `BufferByteIterator` tracks how many bytes `nlohmann::json::sax_parse()` has
   consumed via a caller-supplied `size_t* pos` counter that is incremented on
   every `operator++`.
2. When `start_object` / `start_array` fires at the element boundary depth, the
   handler records `elem_start_ = *parser_pos_ - 1` (the opening `{` or `[` was
   the last byte consumed).
3. All SAX events inside the element are ignored — the handler just increments a
   nesting depth counter and returns `true`.
4. When the matching `end_object` / `end_array` fires and `elem_depth_` reaches
   zero, `sliceBuffer(*slices_, elem_start_, *parser_pos_)` copies the byte range
   `[elem_start_, parser_pos_)` from the non-contiguous slab chain into a
   `std::string`. That string is handed directly to `store_.store()`.

`sliceBuffer` walks `RawSliceVector`, skipping slices before `start`, copying
the overlapping region from each, stopping once `end` is reached. The output
string is `reserve`d to the exact final size before any copying, so there is
exactly one allocation and one or more `memcpy` calls (one per slice that
overlaps the range).

**Why this is strictly better than `SubtreeSerializer` (and `SubtreeBuilder`):**

| | `SubtreeBuilder` | `SubtreeSerializer` | Byte-range capture |
|---|---|---|---|
| DOM construction | yes (per element) | no | no |
| Per-event work inside element | full DOM node alloc | string append + escaping | `++elem_depth_` only |
| Output normalization | yes (`dump()` re-encodes) | yes (re-escapes strings) | **none** — raw bytes |
| Per-element heap alloc | `unique_ptr` per element | none (value member) | none |
| Allocation on capture | `dump()` output string | growing string (reallocs) | one exact-sized `reserve` |

Because all SAX events inside a captured element are discarded (depth counter
only), the work per byte inside any `messages[]` element is a single branch and
counter increment — significantly cheaper than the per-event string-append path
of `SubtreeSerializer`. The stored bytes are bit-for-bit identical to what
appeared in the original request body: no unicode normalization (`A` stays
`A`), no whitespace changes, no `\b`/`\f` edge cases.

### `InferenceSAXHandler` — OpenAI REST body parsing

Handles `POST /v1/chat/completions`, `/v1/completions`, `/v1/embeddings`, etc.

| Depth | Action |
|---|---|
| depth=1 scalar | Extract `model`, `stream`, `temperature`, `top_p`, `max_tokens`, `n`, `seed`, `stop` directly into `InferencePayload` / `AiRequest` |
| depth=2 array open (`messages` or `tools`) | Set `in_messages_` / `in_tools_` flag |
| depth=3 element open (`{` or `[`) | Record `elem_start_ = *parser_pos_ - 1`; set `capturing_element_ = true`, `elem_depth_ = 1` |
| all events while `capturing_element_` | Increment/decrement `elem_depth_` on object/array events; all scalar events return `true` immediately |
| depth=3 element close (`elem_depth_` reaches 0) | `sliceBuffer(*slices_, elem_start_, *parser_pos_)` → `store_.store()` → push `PayloadRef` into `payload_.messages` or `payload_.tools` |
| all other keys/values | Preserved via `residual_params` (the full body buffer stored verbatim) |

After SAX parse completes, `residual_params` receives the entire body via
`store.store(body_buffer_, ...)` — a zero-copy slab transfer for large payloads.
This preserves fields the handler did not extract (e.g. `response_format`,
`tool_choice`, `stream_options`) so the re-encoder can round-trip them faithfully.

### `AgentSAXHandler` — JSON-RPC 2.0 body parsing

Handles MCP and A2A agent requests.

| Field | Extraction |
|---|---|
| `id` | Stored as `request_.jsonrpc_id` (string or number) |
| `method` | Stored as `request_.rpc_method`; triggers re-classify to determine `AgentInvocation` |
| `params` | Captured via byte-range slicing (`params_start_` recorded at opening `{`/`[`, raw bytes sliced at close); `AgentBodyParser::finish()` does one `json::parse()` of the exact-byte string for `populateParams()`, then moves the string into the store for `params_raw` — no DOM held during the SAX pass, bytes identical to input |

Two classification passes:
1. **Headers-time** (`onHeaders`): classify by HTTP method + path alone (no body yet); determines if Inference, AgenticMcp, or AgenticA2a.
2. **Body-time** (`AgentBodyParser::finish`): once `rpc_method` is known, re-classify to determine the specific `AgentInvocation` enum value.

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

### `PayloadStore` — storage backend interface

```cpp
class PayloadStore {
  virtual PayloadRef store(std::string data,       PayloadKind kind) = 0;
  virtual PayloadRef store(Buffer::Instance& data, PayloadKind kind) = 0;
  virtual void       fetch(const PayloadRef& ref,  FetchCallback cb) = 0;
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

`store()` runs on the event loop thread and writes via `memcpy` into the mmap
region. Writes to newly-allocated pages (immediately after `ftruncate`) cause
write page faults, but these are fundamentally cheaper than read page faults:

- **Read fault on an evicted page**: the kernel must re-read from disk — actual
  I/O, potentially milliseconds.
- **Write fault on a new page**: the kernel zero-initializes a fresh physical
  page and maps it in — no disk I/O, just memory allocation.

Writes happen during `onEndStream()` body parsing, immediately after bytes
arrive. The just-`ftruncate`'d pages are brand-new; the OS has had no time to
evict them. The eviction-under-memory-pressure risk that motivates async reads
does not apply.

Making writes async would also require pre-reserving arena offsets, holding the
extracted data in a second temporary buffer which defeating the whole point of 
the mmap offloadm while the async copy races ahead, and stalling or buffering
SAX output — adding significant complexity for no practical benefit.

### External Cache Consideration

If the backing store were an external cache (Redis, memcached, any remote store)
rather than a local mmap file, both the write and read paths would need async
treatment — and the write path would additionally require a secondary buffer.

**Why writes need a secondary buffer**

With mmap, `store()` is synchronous: `memcpy` into the mapped region completes
before the function returns, so a valid `PayloadRef::External{offset, length}`
can be handed back to the SAX parser immediately. The arena offset is stable the
moment the write finishes.

With an external cache the write is a network round-trip. The event loop cannot
block waiting for it, so `store()` cannot return a valid cache key synchronously.
The data must live somewhere while the async write is in flight — that is the
secondary buffer:

```
store() called by SAX parser
  │
  ├─ copy data into heap Buffer::OwnedImpl   ← secondary (temporary) buffer
  ├─ return PayloadRef::Buffered immediately ← SAX parser continues
  └─ fire async write to external cache
        │
        └─ on write confirm: upgrade ref Buffered → External{cache_key}
```

The ref starts as `Buffered` (heap copy) and is upgraded to `External` once the
write confirms. If the write fails it stays `Buffered` — already a valid fallback
the rest of the pipeline handles.

**Comparison: mmap vs external cache**

| | mmap | external cache |
|---|---|---|
| Write | synchronous `memcpy`, no extra allocation | async network write + heap copy as staging buffer |
| Read | async only when page evicted (uncommon) | async on every read (every access is a network round-trip) |
| Memory | pages evictable by OS; physical RAM reclaimed under pressure | heap copy lives until write confirms; cache holds a second copy |

mmap threads the needle: writes are fast enough to be synchronous (no secondary
buffer needed), the OS handles eviction transparently, and reads are only async
when memory pressure actually forces a page out. An external cache provides
durability and cross-process sharing, but pays the secondary buffer cost on every
write and the async cost on every read regardless of memory pressure.

---

### Alternative considered
TODO(tyxia): RapidJSON fastest otpion but it has a history of security vulnerabilities. Have optimized the nlohmann SAX to avoid DOM, intestigate further. 

## End-to-end data flow

```
decodeHeaders()
  └─ RequestDecoder::onHeaders()
       ├─ classify(method, path) → ProtocolKind
       └─ allocate InferenceBodyParser or AgentBodyParser

decodeData() [called 1..N times]
  └─ RequestDecoder::onData(chunk)
       └─ body_buffer_.add(chunk)        ← zero-alloc append to slab chain

decodeTrailers() / end_stream
  └─ RequestDecoder::onEndStream()
       └─ InferenceBodyParser::finish()  (or AgentBodyParser::finish())
            ├─ getRawSlices() → BufferByteIterator (no copy)
            ├─ sax_parse(begin, end, &handler)  ← pos counter incremented each byte
            │    ├─ scalar fields → InferencePayload / AiRequest directly
            │    └─ messages[i] / tools[i] — byte-range capture:
            │         record elem_start_ = *pos - 1 at opening { or [
            │         ignore all events inside (depth counter only)
            │         at close: sliceBuffer(slices, elem_start_, *pos)
            │         → store_.store(raw_bytes)
            │              ├─ size ≤ threshold → PayloadRef::Inline
            │              └─ size >  threshold → PayloadRef::External
            │                                      (offset+len into mmap file)
            └─ residual_params = store_.store(body_buffer_)   ← zero-copy move

dispatch
  └─ filter.cc::dispatch()
       └─ dispatcher.post()            ← re-entrancy guard
            └─ prefetchExternalPayloadRefs()
                 ├─ Inline/Buffered refs → on_done() immediately
                 └─ External refs → fetchAsync() × N (detached threads + pread)
                                     └─ dispatcher.post(on_done) when all complete
                                          └─ doDispatch()

encode
  └─ RequestEncoder / AnthropicRequestEncoder
       └─ convertPayloadRefToString(ref, request)
            ├─ Inline/Buffered → ref.toString()   ← all External already upgraded
            └─ External        → store.fetch()    ← only if prefetch was skipped
```

---

## Configuration

The filter uses `MmapPayloadStore` by default, constructed with:

```cpp
MmapPayloadStore payload_store_{"/tmp", config->decoderConfig().max_inline_bytes};
```

`max_inline_bytes` (default 4096) controls the inline/offload threshold.
Fields whose serialized JSON size is at or below this value are stored `Inline`;
larger fields are offloaded to the backing file as `External` refs.

---

## Tests

| Test file | What it covers |
|---|---|
| `test/.../payload_store_test.cc` | 23 unit tests: `PayloadRef::External` accessors, inline threshold boundary, fetch roundtrip for strings and multi-slice buffers, capacity doubling (forces `mremap`/`munmap+mmap` path), `InMemoryPayloadStore` regression |
| `mcp_auth_filter_integration_test.cc` | 5 integration tests: `makeLargeToolsCallBody` pads JSON-RPC bodies past 4096 B so the SAX parser stores `messages[]` elements as `External` refs; verifies auth, routing, and tool-name extraction work end-to-end |
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
