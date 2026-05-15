# Request Parsing — simdjson On-Demand Design

## Why a new parser

The previous implementation used `nlohmann::json::sax_parse()` with a custom
`BufferByteIterator` that walked Envoy's `Buffer::OwnedImpl` slab chain
directly. The SAX approach was chosen to avoid a full DOM parse, and byte-range
capture (`sliceBuffer`) extracted element raw bytes without re-serialization.

One problem remained. nlohmann's lexer is **eager**: before calling any SAX
callback, it fully tokenizes each JSON value — which means allocating a
`std::string` for every string value it encounters, regardless of whether the
handler discards it. For a multimodal inference request carrying a 10 MB
base64-encoded image:

```json
{
  "messages": [{
    "role": "user",
    "content": [{"type": "image_url", "image_url": {"url": "data:image/jpeg;base64,<10MB>"}}]
  }]
}
```

Even with `capturing_element_ = true` (which makes the handler return
immediately), nlohmann has already allocated a 10 MB `std::string` before the
`string()` callback is called. That allocation is purely transient — it exists
for exactly one callback invocation and is then destroyed. There is no way to
intercept it from the public SAX API.

### Why not just suppress large elements?

The element's total size is only knowable after finding its closing `}` — which
requires parsing the interior. A pre-pass brace scanner could find element
boundaries before nlohmann sees them, but nlohmann still reads every byte as
part of SAX iteration, so string allocations inside a skipped element still
happen. Suppressing the final `sliceBuffer` copy avoids one allocation, but
the transient string allocations inside the element remain.

### Why not RapidJSON?

RapidJSON's SAX `string` callback passes `(const char* str, SizeType length,
bool copy)`. When `copy = false` (no escape sequences in the string), it is a
zero-copy pointer into the source buffer — no allocation. Base64 content is
always pure ASCII, so `copy` would be `false` for the image case.

However:

- RapidJSON is not in the Envoy dependency tree. Adding it is equivalent work
  to adding simdjson.
- RapidJSON SAX has no native "skip subtree" mechanism. Returning `false` from
  any callback aborts the entire parse. Depth-tracking to skip large elements
  requires the same manual code as the current nlohmann approach.
- RapidJSON with `copy = true` (escaped strings) still allocates, so it only
  partially solves the problem.

### simdjson on-demand

simdjson's on-demand API is **genuinely lazy**: the document is parsed field by
field as the caller iterates. Values that are never accessed are never lexed.
`raw_json()` returns a `std::string_view` into the source buffer for any value
— including arrays and objects — without parsing their interior at all.

For the image case:

```
for (auto elem : messages_array) {
    std::string_view raw = elem.raw_json();   // ← zero allocation; no interior parse
    if (raw.size() <= max_element_capture_bytes) {
        store.store(std::string(raw), ...);   // ← one copy into the store
    }
    // else: just continue — simdjson advances past the element, nothing allocated
}
```

The 10 MB base64 string is never seen as a `std::string`. If the element is
below the capture threshold, the one copy is `std::string(raw)` — identical
cost to the old `sliceBuffer()` path. If it exceeds the threshold the whole
element is skipped at zero allocation cost.

---

## simdjson concepts used

### `padded_string`

simdjson's SIMD routines read ahead by up to `SIMDJSON_PADDING` bytes (64 B on
most architectures) past the end of the document to avoid branch-heavy end-of-
buffer checks. `simdjson::padded_string` allocates a buffer that is
`input_length + SIMDJSON_PADDING` bytes, copies the input in, and zero-fills
the tail. The document pointer passed to the parser is safe to overread.

### `ondemand::parser`

A reusable parser object that holds a small pre-allocated string builder. It
can be reused across documents but can only hold one live document at a time.
In this codebase a fresh `parser` is created in each `finish()` call — the
overhead is negligible (a single heap allocation of ~2 KB).

### `ondemand::document` / `ondemand::object`

The document object is a cursor into the padded buffer. It has no heap
allocation beyond what the `parser` already holds. Iterating over an object's
fields advances this cursor; fields that are not accessed in the iteration body
are automatically skipped when the iterator moves to the next field.

### `field.unescaped_key()`

Returns a `simdjson_result<std::string_view>`. For JSON keys that are plain
ASCII with no escape sequences (all keys in OpenAI / JSON-RPC bodies), this is
a zero-copy view into the padded buffer — no string allocation.

### `value.raw_json()`

Returns a `simdjson_result<std::string_view>` pointing at the raw JSON bytes of
any value — string, number, object, or array — in the padded buffer. The view
does not include trailing commas or whitespace. The call marks the value as
consumed, so the iterator correctly advances past it on the next iteration step.

**Lifetime**: the returned `std::string_view` is valid only while the
`padded_string` that backs the document is alive. In `finish()` both `padded`
and all derived views have the same stack frame lifetime. Any bytes that need to
survive beyond `finish()` must be copied.

---

## Buffer lifecycle inside `finish()`

```
Buffer::OwnedImpl body_buffer_     (slab chain, survives full request lifetime)
         │
         │  getRawSlices() + memcpy     ← ONE copy: slab chain → contiguous buf
         ▼                                 tail zero-initialised for SIMD overread
 char[] buf[body_size + 64 B]       (owned by finish() stack frame)
         │
         │  padded_string_view(buf, body_size, body_size + SIMDJSON_PADDING)
         ▼
 padded_string_view padded           (non-owning view into buf; zero extra allocation)
         │
         │  parser.iterate(padded)
         ▼
 ondemand::document doc              (cursor into buf; zero heap allocation)
         │
         ├─ scalar fields:  std::string(view_into_buf)  → copied into payload
         │
         └─ elements:  std::string(raw_json())  → copied into PayloadStore   [inference]
            params_raw / arguments:  store.slice(residual_ref, offset, len)  [agent]
                                                 ↕
                                 body_buffer_ → store.store(body_buffer_)
                                                ↑ residual_params
```

After `finish()` returns:
- `buf` and `padded` are destroyed (stack unwinds).
- `body_buffer_` is still alive inside `InferenceBodyParser` / `AgentBodyParser`;
  `residual_params` holds a `PayloadRef` into the store that owns the bytes.
- Scalar strings (`model`, `rpc_method`, etc.) are owned by `AiRequest`.
- Element `PayloadRef`s are owned by `InferencePayload::messages` / `tools`.
- Agent `params_raw` and `arguments` are `External{offset, len}` slice refs into
  the same mmap region as `residual_params` — zero extra bytes in the store.

The total heap owned after `finish()` is:
```
residual_params   ← whole body in PayloadStore (mmap or heap)
+ scalar strings  ← small strings in AiRequest / InferencePayload fields
+ element copies  ← std::string(raw) for each captured element (Tier 1, inference only)
```

No transient allocations survive `finish()`.

---

## `InferenceBodyParser`

Handles `POST /v1/chat/completions`, `POST /v1/completions`, `POST /v1/embeddings`, etc.

### Single-pass field iteration

```cpp
for (auto field : obj) {
    std::string_view key;
    if (field.unescaped_key().get(key)) continue;   // skip malformed keys

    if      (key == "model")       { ... }
    else if (key == "stream")      { ... }
    else if (key == "max_tokens")  { ... }
    else if (key == "temperature") { ... }
    else if (key == "top_p")       { ... }
    else if (key == "seed")        { ... }
    else if (key == "n")           { ... }
    else if (key == "stop")        { ... }           // string or array
    else if (key == "messages" || key == "tools") { ... }
    // all other keys: simdjson auto-skips — zero allocation
}
```

One pass, linear in the number of fields. Unknown fields (e.g. `response_format`,
`tool_choice`, `stream_options`) cost one key comparison and are then skipped
without any value access.

### `stop` — dual-type handling

`stop` can be either a string or an array of strings per the OpenAI spec:

```cpp
auto val = field.value();
simdjson::ondemand::json_type type;
if (!val.type().get(type)) {
    if (type == simdjson::ondemand::json_type::string) {
        std::string_view sv;
        if (!val.get_string().get(sv)) payload.sampling.stop.push_back(std::string(sv));
    } else if (type == simdjson::ondemand::json_type::array) {
        for (auto elem : val.get_array()) { ... }
    }
}
```

`val.type()` peeks at the leading byte of the value without consuming it, so
the value can still be accessed via `get_string()` or `get_array()` after the
type check.

### Element capture (`messages` / `tools`)

```cpp
if (!capture_elements) continue;   // Tier 2: simdjson auto-skips, zero allocation

simdjson::ondemand::array arr;
if (field.value().get_array().get(arr)) continue;
for (auto elem : arr) {
    std::string_view raw;
    if (elem.raw_json().get(raw)) continue;   // raw = JSON bytes of entire element
    PayloadRef ref = store.store(std::string(raw), PayloadKind::JsonObject);
    (is_messages ? payload.messages : payload.tools).push_back(std::move(ref));
}
```

`raw_json()` on an array element returns the raw JSON of that element —
`{...}` or `[...]` inclusive — without parsing its interior. For a 10 MB
base64 image element, this is a single `memcpy` of the raw bytes, not a 10 MB
string construction.

When `capture_elements` is `false` (Tier 2), `continue` is hit before
`field.value()` is ever called. simdjson automatically skips the `messages`
array value when the outer object iterator advances to the next field. No bytes
inside the array are lexed; no allocation of any kind occurs.

---

## `AgentBodyParser`

Handles JSON-RPC 2.0 bodies (MCP, A2A).

### Two-pass params extraction

The outer body is parsed in one pass to extract `id`, `method`, and the raw
bytes of `params`. Then, if the body is small enough to capture and the
invocation type is known, a second simdjson parse of the `params` bytes
extracts invocation-specific fields.

**Why two passes?**

simdjson on-demand consumes values as you access them. Once `raw_json()` is
called on the `params` value, the cursor has advanced past it — the interior
cannot be iterated again. Conversely, if `params` is iterated first (to extract
`name`, `arguments`, etc.), `raw_json()` cannot recapture the bytes afterward.

The two-pass design avoids this by:
1. First pass (outer doc): `raw_json()` on `params` → record `params_start` / `params_len` (byte offset and length within the padded body buffer — no copy).
2. Re-classify using `rpc_method` to determine `AgentInvocation`.
3. Store `residual_params` (whole body) first, then create `params_raw` as `store.slice(residual_params, params_start, params_len)` — zero extra bytes for `External` refs.
4. Second pass (`populateParams`): parse the params bytes in-place from the padded body buffer; sub-objects (`arguments`, `capabilities`) are created as `store.slice(residual_params, body_offset, len)` rather than independent copies.

```
outer doc (one pass)
  ├─ id      → request.jsonrpc_id
  ├─ method  → request.rpc_method
  └─ params  → raw_json() → record params_start, params_len (no copy)

re-classify(rpc_method) → AgentInvocation

store residual_params = store.store(body_buffer_)         ← whole body
params_raw            = store.slice(residual_params, params_start, params_len)

populateParams(buf + params_start, params_len, params_start, residual_params)
  ├─ ToolsCall:    name → tool_name (scalar copy)
  │                arguments → store.slice(residual_params, body_off, len)
  ├─ ResourcesRead/...: uri → resource_uri (scalar copy)
  ├─ PromptsGet:   name → prompt_name (scalar copy)
  │                arguments → store.slice(residual_params, body_off, len)
  ├─ CompletionComplete: ref → completion_ref (scalar copy)
  └─ Initialize:   capabilities → store.slice(residual_params, body_off, len)
```

For `MmapPayloadStore`, `store.slice()` on an `External` ref is pure offset
arithmetic — `makeExternal(parent.offset + offset, len)` — zero allocation,
zero copy. `params_raw` and `arguments` occupy 16 bytes each (a `uint64_t`
offset + a `size_t` length), not a copy of the params bytes.

The second parse allocates one `padded_string` of `params_len + 64 B` for
simdjson's SIMD overread safety. This is freed when `populateParams` returns.

### `id` — string-or-number handling

JSON-RPC allows `id` to be either a string or an integer. simdjson's type
dispatch handles both:

```cpp
auto val = field.value();
simdjson::ondemand::json_type type;
if (!val.type().get(type)) {
    if (type == simdjson::ondemand::json_type::string) {
        std::string_view sv;
        if (!val.get_string().get(sv)) request.jsonrpc_id = std::string(sv);
    } else if (type == simdjson::ondemand::json_type::number) {
        int64_t v;
        if (!val.get_int64().get(v)) request.jsonrpc_id = std::to_string(v);
    }
}
```

---

## Memory model

### Allocation comparison: nlohmann SAX vs simdjson on-demand

For a body containing a 10 MB base64 image element:

| Step | nlohmann SAX | simdjson on-demand |
|------|--------------|-------------------|
| Buffer body | slab chain (zero-copy append) | slab chain (zero-copy append) |
| Contiguous copy for parse | none — `BufferByteIterator` walks slabs in place | one — `new char[body + 64 B]()`, slab chain `memcpy`'d directly in |
| Padded copy for SIMD | n/a | none — `padded_string_view` points into the allocation above |
| Scalar keys | `std::string` alloc per key | `string_view` into padded buf — zero alloc |
| String value inside skipped element | **10 MB `std::string` allocated by lexer** | nothing — never lexed |
| Captured element copy | `sliceBuffer()` → `std::string` (one copy) | `std::string(raw_json())` (one copy) |

simdjson trades the zero-copy slab iteration for one up-front copy of the full
body into a pre-padded allocation, but eliminates the transient per-string-value
allocations that nlohmann's lexer made unavoidable.

For a typical 200 KB chat body with no images, the net difference is small.
For vision requests with large base64 payloads, simdjson eliminates O(image_size)
transient allocations that would otherwise spike RSS on every request.

### Tier summary

| Tier | Condition | Padded buffer | Element copies | Peak live |
|------|-----------|---------------|----------------|-----------|
| 1 | body ≤ `max_element_capture_bytes` | body + 64 B | Σ element sizes → store | ≈ 1.1× body + elements |
| 2 | body ≤ `max_body_bytes` | body + 64 B | none | ≈ 1.1× body |
| 3 | body > `max_body_bytes` | never created | n/a | ≤ `max_body_bytes` |

The 1.1× factor is the single pre-padded allocation (`body + 64 B`), a
stack-frame local freed when `finish()` returns. The old 2.1× figure predated
the `padded_string_view` fix that eliminated the intermediate `body_str` copy.
Long-lived allocations after `finish()` are scalar strings in `AiRequest` and
element `PayloadRef`s in the store.

---

## The contiguous-input constraint

### Why the copy is unavoidable

Envoy's `Buffer::OwnedImpl` is a **slab chain** — an iovec-style list of
non-contiguous memory regions. Bodies that arrive in multiple `decodeData`
chunks, or are large enough to span multiple slabs, are physically scattered
in memory. No single `{ptr, len}` pair describes the full body.

simdjson needs contiguous input for two independent reasons, either of which
alone would force a copy:

**1. SIMD loads require contiguous 64-byte chunks.**

simdjson's Stage 1 structural pass loads 64 bytes at a time into SIMD
registers:

```
vload(ptr)  →  reads [ptr, ptr+64) as one register
```

At a slab boundary `ptr+63` may be in a different physical page from `ptr`.
The SIMD load reads garbage or segfaults. This is not workable around without
a per-boundary scalar fallback that would add branch overhead and code
complexity.

**2. `string_view` results require the source to be contiguous.**

simdjson on-demand returns `string_view{const char*, size_t}` for every value
— keys, scalars, `raw_json()` sub-trees. A `string_view` is two pointers into
a single flat buffer. A JSON string value that straddles a slab boundary
cannot be represented as a `string_view` without first copying the bytes into
a contiguous region. If the API returned an owned `std::string` for
cross-boundary values, it would defeat the zero-allocation contract for exactly
the large values (base64 images, tool schemas) that motivated the migration.

**3. Even a single-slab body still needs a copy for padding.**

simdjson's SIMD routines overread `SIMDJSON_PADDING` (64 B) bytes past the
end of the document to avoid branch-heavy end-of-buffer checks. A slab's
capacity ends at its last byte; reading past that is undefined behaviour.
There is no Envoy API to check whether a slab has spare capacity, or to
zero-fill it in-place safely.

nlohmann's `BufferByteIterator` avoids all of this because its SAX parser is
a state machine that consumes one character at a time. Each byte is processed
independently; there is no assumption about memory contiguity between
consecutive bytes. The copy is the price of simdjson's SIMD speed.

### Why `finish()` always has the full body before parsing

`finish()` is called from `onEndStream()`, not from `decodeData()`. This is a
deliberate security constraint: AI requests are frequently body-signed or
subject to body-aware authorization.

- **Body-signed requests** (HMAC, AWS Signature V4): the auth signature covers
  the entire body. Acting on parsed fields from partial, unverified bytes is a
  security vulnerability — an attacker can craft a body whose early bytes look
  authorized but whose later bytes change meaning.
- **ext_authz with body inspection**: the external auth service authorizes
  based on body content (model name, tool name, resource URI). It needs the
  complete body.

Parsing after `onEndStream()` guarantees that authentication has completed
over the full body before any extracted field value is acted upon. This also
means both nlohmann SAX and simdjson on-demand are used in identical fashion
here — neither streams incrementally despite their different capabilities.

### What the copy actually costs

The copy is one `memcpy` of `body_size` bytes from the slab chain into `buf`.
At sustained DRAM bandwidth (~10 GB/s on modern ARM/x86):

| Body size | Copy time |
|---|---|
| 10 KB | < 1 µs |
| 50 KB | ~5 µs |
| 200 KB | ~20 µs |
| 1 MB | ~100 µs |
| 4 MB | ~400 µs |

These are worst-case estimates for cold memory. For bodies that fit in L3 cache
(< ~8 MB on most server CPUs) the effective bandwidth is higher and latency
lower. The copy is a single sequential scan — the most cache-friendly memory
access pattern possible.

### Is the copy worth it

The copy eliminates nlohmann's per-value transient allocations. The comparison
is not copy-vs-zero-copy but copy-vs-heap-allocator-churn:

| Workload | nlohmann cost | simdjson cost | Verdict |
|---|---|---|---|
| Small text-only chat (< 10 KB) | Near-zero transients; zero copy | < 1 µs copy; zero transients | **nlohmann slightly better** |
| Typical chat (50–200 KB), text | Small transients; zero copy | 5–20 µs copy; zero transients | **Roughly break-even** |
| Vision, 10 MB base64, Tier 2 | 10 MB `std::string` allocated per image string field + heap fragmentation under concurrency | ~1 ms copy; image bytes never lexed | **simdjson clearly better** |
| Vision, 10 MB image, Tier 1 | 10 MB transient + 10 MB `sliceBuffer` copy | ~1 ms copy + 10 MB element copy | **simdjson better** (no transient spike) |
| Agent, any params size | 3× params bytes in store (independent copies) + DOM for params parse | 1× body in store; `params_raw` and `arguments` are zero-byte slice refs; no DOM | **simdjson clearly better** |

The copy is a fixed O(body_size) sequential cost. nlohmann's transient
allocations are O(Σ string_sizes) and involve the heap allocator under
concurrent load — which causes lock contention, fragmentation, and RSS spikes
that grow with concurrency, not just body size. For the multimodal and
agentic workloads this proxy is built for, simdjson is the right tradeoff.

---

## Bazel integration

### Repository

simdjson distributes a single-header amalgamation (`simdjson.h` + `simdjson.cpp`)
via a `singleheader.zip` release asset. This mirrors how `simdutf` is already
integrated in Envoy:

```
bazel/repository_locations.bzl  ← version + SHA256 + URL
bazel/repositories.bzl          ← _simdjson() + call site
bazel/external/simdjson.BUILD   ← cc_library(name="simdjson", srcs=[".cpp"], hdrs=[".h"])
```

### ARM NEON pragma

simdjson's ARM NEON code paths use C-style casts inside macros defined in
`arm_neon.h` (the platform intrinsics header). Envoy builds with
`-Werror,-Wold-style-cast`, which treats these as errors.

The casts are in `arm_neon.h`, not in simdjson's own code. Adding
`-Wno-old-style-cast` to the simdjson `cc_library` target suppresses warnings
when compiling `simdjson.cpp` but has no effect on consumer translation units
that `#include "simdjson.h"`.

The fix is a pragma-scoped suppression precisely around the include in
`request_decoder.cc`:

```cpp
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#include "simdjson.h"
#pragma GCC diagnostic pop
```

Both GCC and Clang honour `#pragma GCC diagnostic`. The suppression is scoped
to the include directive only — all subsequent code in the TU retains the full
`-Wold-style-cast` check.

---

## Error handling

All simdjson API calls return `simdjson_result<T>`. The `.get(out)` pattern
returns a non-zero `simdjson_error` on failure; zero means success.

```cpp
if (auto err = parser.iterate(padded).get(doc); err) {
    return absl::InvalidArgumentError(
        absl::StrCat("JSON parse error: ", simdjson::error_message(err)));
}
```

For field-level access in the iteration body, failures are soft: a bad key or
wrong-type value causes `continue` to the next field rather than aborting the
whole parse. This mirrors the previous nlohmann SAX approach where type
mismatches in the `boolean` / `number_integer` callbacks were silently ignored.

---

## Tests

The three-tier behaviour is validated by `request_decoder_test.cc`:

| Test | Body size | Expected |
|------|-----------|----------|
| `SmallBody_ElementsCaptured` | < soft limit | `messages` populated with one `PayloadRef` |
| `LargeBody_ScalarsOnlyNoElements` | > soft limit, < hard limit | `messages` empty; `model` + `stream` still set |
| `ExceedsHardLimit_ReturnsError` | > hard limit | `onData` returns `ResourceExhausted` |
| `SmallBody_ParamsCaptured` | < soft limit | `params_raw` non-empty; `tool_name` extracted |
| `LargeBody_ScalarsOnlyNoParams` | > soft limit, < hard limit | `params_raw` empty; `rpc_method` still set |
| `ExceedsHardLimit_ReturnsError` (agent) | > hard limit | `onData` returns `ResourceExhausted` |

Thresholds in the tests are set to 100 B (soft) and 500 B (hard) so payloads
fit in any tier without synthesising multi-megabyte JSON.

---

## nlohmann SAX vs simdjson on-demand — detailed tradeoff

### A — Body ingestion and parse-input preparation

| Dimension | nlohmann SAX | simdjson on-demand |
|---|---|---|
| **Up-front contiguous copy** | None — `BufferByteIterator` walks slab chain in place | One — `new char[body + 64 B]()` allocated; slab chain copied in directly via `getRawSlices()` |
| **Padding / SIMD requirement** | None | `SIMDJSON_PADDING` (64 B) of zeros appended; SIMD routines overread past document end |
| **Parse-input type** | `BufferByteIterator` (non-owning, zero-copy) | `padded_string_view` (non-owning view into the pre-padded allocation) |
| **Peak transient bytes during `finish()` — no large strings** | ~1× body (slab chain read in place) | ~1.1× body (one padded copy; freed on `finish()` return) |
| **Peak transient bytes during `finish()` — 10 MB base64, Tier 2** | ~1× body + up to 10 MB transient per `string()` callback — spike is unavoidable | ~1.1× body, flat — large string never lexed, no spike |
| **Peak transient bytes during `finish()` — 10 MB image, Tier 1** | ~1× body + 10 MB transient + 10 MB `sliceBuffer` copy | ~1.1× body + 10 MB `std::string(raw)` for captured element |
| **All transient bytes freed after `finish()`** | Yes | Yes |

### B — Per-field parsing costs

| Dimension | nlohmann SAX | simdjson on-demand |
|---|---|---|
| **DOM construction** | None during SAX pass; `populateParams()` calls `json::parse()` → full DOM of params object | None at any stage; `populateParams()` also uses on-demand cursor — no DOM ever |
| **Per-key allocation** | `std::string` heap allocation per key, before handler fires | `field.unescaped_key()` returns `string_view` into padded buf — zero alloc |
| **Per-string-value allocation** | `std::string` always, inside lexer, unconditionally, before handler sees it | `get_string()` returns `string_view`; skipped values allocate nothing |
| **Skipping a 10 MB base64 string value** | 10 MB `std::string` allocated and freed immediately — cannot be suppressed via public SAX API | 0 bytes — cursor jumps past the value |
| **Type peeking (`stop`: string vs array)** | Separate `string()` and `start_array()` callbacks drive a state flag | `val.type()` peeks leading byte without consuming the value; `get_string()` / `get_array()` still usable after |
| **Dual-type `id` (string vs integer, JSON-RPC)** | Separate `string()` and `number_integer()` SAX callbacks; state machine needed | `val.type()` dispatch, then `get_string()` or `get_int64()` — no handler state |
| **Unknown field handling** | SAX fires all callbacks for every field, including value bytes; handler explicitly ignores | Cursor auto-advances past the value — zero bytes inside the value are accessed |

### C — Element and params capture

| Dimension | nlohmann SAX | simdjson on-demand |
|---|---|---|
| **Skipping `messages` / `tools` array (Tier 2)** | SAX events fire through every byte of the array; per-string transient allocs inside still happen | `continue` before `field.value()` — simdjson skips the whole array; zero bytes lexed, zero allocated |
| **Capturing a large element (Tier 1)** | `sliceBuffer()` walks `RawSliceVector`, one exact-size `reserve` + N `memcpy` (one per slab) | `raw_json()` gives `string_view`; one `std::string(raw)` copy into the store — equivalent cost |
| **Captured bytes fidelity** | Bit-for-bit identical to request body | Bit-for-bit identical to request body |
| **Agent params — outer parse** | One SAX pass; `elem_start_` / `elem_end_` byte-range positions recorded at `{` / `}` events | One on-demand pass; `raw_json()` on `params` records `params_start` / `params_len` (pointer arithmetic into padded buf) — no copy |
| **Agent params — `populateParams()` parse** | `json::parse(params_str)` — builds full DOM of the params object | Fresh `ondemand::parser` on a `padded_string` of params bytes — cursor only, no DOM; freed on return |
| **Agent params — `params_raw` storage** | One copy: `sliceBuffer()` into `params_str` → stored as independent entry | Zero extra bytes: `store.slice(residual_params, params_start, params_len)` → `External{offset, len}` into existing mmap region |
| **Agent params — `arguments` / `capabilities` storage** | One copy per sub-object: `sliceBuffer()` range → stored as independent entry | Zero extra bytes: `store.slice(residual_params, body_off, len)` → `External{offset, len}` — pure offset arithmetic |
| **`ondemand::parser` instances per `finish()`** | n/a | Two (outer + `populateParams`) for agents; one for inference — each ~2 KB heap alloc |

### D — Post-`finish()` storage redundancy

| Stored ref | Storage (MmapPayloadStore) | Bytes in store | Who holds it |
|---|---|---|---|
| `messages[i]` / `tools[i]` | `External{off, E}` — independent entry written by element capture | E bytes per element | `InferencePayload` |
| `params_raw` | `External{residual.off + params_start, P}` — slice of `residual_params` | 0 extra bytes | `AgentPayload` |
| `arguments` / `capabilities` | `External{residual.off + body_off, A}` — slice of `residual_params` | 0 extra bytes | `AgentPayload` |
| `residual_params` | `External{off, B}` — whole body written once | B bytes | Both payload types |

**Agent requests (after `slice()` fix):** `params_raw` and `arguments` are `External{offset, len}` descriptors pointing into the same mmap region as `residual_params`. Zero additional bytes written. For a 500 KB `arguments` blob: the store holds exactly B bytes (the full body), not 3× B.

```
Before fix:  arguments (500 KB) + params_raw (501 KB) + residual_params (502 KB) = 1.5 MB
After fix:   residual_params (502 KB) + two External{uint64+size_t} structs     = 502 KB
```

**Inference requests:** `messages[i]` elements are still independent copies in the store — each element is written as a separate entry. Element bytes appear twice: once in the individual ref and once inside `residual_params`. This is the remaining redundancy; a future `slice()` call on inference elements would require tracking each element's byte offset during the `raw_json()` call, analogous to the agent fix.

**`InMemoryPayloadStore` (tests):** `slice()` materializes a substring via `parent.toString().substr(offset, len)` and stores it as a new entry — no special zero-copy path. Functionally correct; production always uses `MmapPayloadStore`.

### E — Code structure and correctness

| Dimension | nlohmann SAX | simdjson on-demand |
|---|---|---|
| **SAX handler state** | ~10 member variables (`depth_`, `in_messages_`, `in_tools_`, `capturing_element_`, `elem_depth_`, `elem_start_`, `parser_pos_`, `slices_`, …) | None — all state implicit in on-demand cursor |
| **Code complexity** | High — depth counter must stay in sync with `capturing_element_` transitions; off-by-one on `elem_start_` silently corrupts captured bytes | Low — `for (auto field : obj)` with `if/else if`; no depth tracking |
| **Parse still runs in Tier 2** | Yes — SAX fires through all bytes; per-string transient allocs still occur inside skipped elements | Yes — field iteration runs; skipped arrays cost zero |
| **Top-level structural error** | SAX returns `false` on first syntax error; parse aborts | `parser.iterate()` returns non-zero `simdjson_error`; surfaced as `InvalidArgumentError` |
| **Field-level type mismatch** | Ignored silently — wrong-type SAX callbacks not dispatched to handler | Soft `continue` — wrong-type `get_T()` fails, field skipped; parse continues |

### F — Infrastructure

| Dimension | nlohmann SAX | simdjson on-demand |
|---|---|---|
| **Dependency** | `@nlohmann_json` — already in Envoy tree | `@simdjson` — new; single-header amalgamation (`simdjson.h` + `simdjson.cpp`), same integration pattern as `simdutf` |
| **ARM NEON `-Wold-style-cast`** | No issue | C-style casts inside `arm_neon.h` macros require `#pragma GCC diagnostic` guard scoped around `#include "simdjson.h"` |
| **Platform SIMD** | None | Auto-selects AVX-512 / AVX2 / SSE4.2 / NEON / fallback at compile time |

### G — Workload summary

| Workload | nlohmann SAX | simdjson on-demand |
|---|---|---|
| Small chat body (< 10 KB), no images | Slightly better — zero body copy | Slightly worse — 64 B padding + one `memcpy` for no practical gain |
| Typical chat body (50–200 KB), text only | Comparable — transient strings small; slab walk saves one copy | Comparable — one copy; zero per-string allocs; net wash |
| Vision request, 10 MB base64 image, Tier 2 | Significantly worse — 10 MB transient `std::string` per image field, unavoidable | Significantly better — image bytes never lexed; transient flat at ~1.1× body |
| Vision request, Tier 1 (element captured) | Both pay one element copy; nlohmann also pays 10 MB transient | Both pay one element copy; simdjson pays nothing extra |
| Agent request, any params size | Independent copies: `arguments` + `params_raw` + `residual_params` = 3× params bytes in store; DOM for params parse | `residual_params` written once; `params_raw` and `arguments` are `External{offset,len}` slices — 0 extra bytes; no DOM |
| Agent request, 500 KB arguments | ≥ 1.5 MB in store (3× copies) + DOM alloc | 502 KB in store (1× body only) + one 500 KB `padded` alloc freed after second parse |
