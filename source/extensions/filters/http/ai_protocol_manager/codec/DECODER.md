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
   `tools[]` entries, `params`) are captured one at a time via `SubtreeBuilder`
   and immediately handed to the store. The full-body DOM never exists.

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

### `SubtreeBuilder` — element-at-a-time capture

`SubtreeBuilder` reconstructs one `nlohmann::json` value from a subsequence of
SAX events. It maintains a `stack_` of in-progress containers (objects/arrays)
and a completed `result_`.

Only **one sub-tree is live in memory at a time**. When `InferenceSAXHandler`
detects the start of a `messages[]` or `tools[]` element it allocates a
`SubtreeBuilder`; when the element closes it calls `store_.store(elem.dump())`
and resets the builder. The full messages/tools array DOM is never in memory
simultaneously.

### `InferenceSAXHandler` — OpenAI REST body parsing

Handles `POST /v1/chat/completions`, `/v1/completions`, `/v1/embeddings`, etc.

| Depth | Action |
|---|---|
| depth=1 scalar | Extract `model`, `stream`, `temperature`, `top_p`, `max_tokens`, `n`, `seed`, `stop` directly into `InferencePayload` / `AiRequest` |
| depth=2 array open (`messages` or `tools`) | Set `in_messages_` / `in_tools_` flag |
| depth=3 element open | Allocate `SubtreeBuilder`, start capturing |
| depth=3 element close | Serialize element, call `store_.store()`, push `PayloadRef` into `payload_.messages` or `payload_.tools` |
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
| `params` | Captured in a `SubtreeBuilder`; then `populateParams()` extracts invocation-specific fields (`tool_name`, `resource_uri`, `prompt_name`, `arguments`, `capabilities`) |

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
may encounter external refs must use `materializeRef(ref, request)` (see below).

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

## `materializeRef` — safe cross-storage accessor

```cpp
// ai_request.h / ai_request.cc
std::string materializeRef(const PayloadRef& ref, const AiRequest& request);
```

Routes through the correct path for every storage variant:

- `Inline` / `Buffered` → calls `ref.toString()` directly.
- `External` → calls `request.payload_store->fetch(ref, ...)` to read from the
  mmap region.

All encoder call sites (`request_encoder.cc`, `anthropic_request_encoder.cc`) use
`materializeRef` rather than `ref.toString()` so they remain correct regardless of
which store the filter is using.

---

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
            ├─ sax_parse(begin, end, &handler)
            │    ├─ scalar fields → InferencePayload / AiRequest directly
            │    └─ messages[i] / tools[i]:
            │         SubtreeBuilder captures element
            │         → store_.store(elem.dump())
            │              ├─ size ≤ threshold → PayloadRef::Inline
            │              └─ size >  threshold → PayloadRef::External
            │                                      (offset+len into mmap file)
            └─ residual_params = store_.store(body_buffer_)   ← zero-copy move

dispatch / encode
  └─ RequestEncoder / AnthropicRequestEncoder
       └─ materializeRef(ref, request)
            ├─ Inline/Buffered → ref.toString()
            └─ External        → store.fetch() → read from mmap region
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
   PANIC. Use `materializeRef(ref, request)` at every encoder call site.
2. The `MmapPayloadStore` instance must outlive all `External` `PayloadRef`s
   created from it. The filter satisfies this: the store is a filter member and
   refs are owned by `AiRequest`, which is destroyed before the filter.
3. A store that failed to mmap still returns valid refs (falling back to
   `Buffered`). Callers never need to check store health.
