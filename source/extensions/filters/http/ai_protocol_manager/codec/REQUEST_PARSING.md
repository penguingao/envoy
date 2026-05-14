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
         │  body_buffer_.toString()          ← contiguous copy; no-op if single slab
         ▼
     std::string body_str           (owned by finish() stack frame)
         │
         │  simdjson::padded_string(body_str.data(), body_str.size())
         ▼
 simdjson::padded_string padded      (padded copy: body_str + SIMDJSON_PADDING zeros)
         │
         │  parser.iterate(padded)
         ▼
 ondemand::document doc              (cursor into padded; zero heap allocation)
         │
         ├─ scalar fields:  std::string(view_into_padded)  → copied into payload
         │
         └─ element/params: std::string(raw_json_view)     → copied into PayloadStore
                                                              ↕
                                              body_buffer_ → store.store(body_buffer_)
                                                             ↑ residual_params (zero-copy slab transfer)
```

After `finish()` returns:
- `body_str` and `padded` are destroyed (stack unwinds).
- `body_buffer_` is still alive inside `InferenceBodyParser` / `AgentBodyParser`;
  `residual_params` holds a `PayloadRef` into the store that owns the bytes now.
- Scalar strings (`model`, `rpc_method`, etc.) are owned by `AiRequest`.
- Element `PayloadRef`s are owned by `InferencePayload::messages` / `tools`.

The total heap owned after `finish()` is:
```
body_buffer_      ← slab chain (zero-copy move into residual_params PayloadStore)
+ scalar strings  ← small strings in AiRequest / InferencePayload fields
+ element copies  ← std::string(raw) for each captured element (Tier 1 only)
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
1. First pass (outer doc): `raw_json()` on `params` → `params_raw_str`.
2. Re-classify using `rpc_method` to determine `AgentInvocation`.
3. Second pass (`populateParams`): parse `params_raw_str` as a new simdjson
   document, iterate its fields to extract invocation-specific scalars and
   sub-objects.

```
outer doc (one pass)
  ├─ id      → request.jsonrpc_id
  ├─ method  → request.rpc_method
  └─ params  → raw_json() → params_raw_str (copy of raw bytes)

re-classify(rpc_method) → AgentInvocation

populateParams(params_raw_str) (second simdjson parse, small object)
  ├─ ToolsCall:             name → tool_name, arguments → store
  ├─ ResourcesRead/...:     uri  → resource_uri
  ├─ PromptsGet:            name → prompt_name, arguments → store
  ├─ CompletionComplete:    ref  → completion_ref
  └─ Initialize:            capabilities → store
```

The second parse is always on a small object (params is never a 10 MB blob),
so the cost is negligible.

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
| Contiguous copy for parse | none (BufferByteIterator walks slabs) | `body_buffer_.toString()` → one copy |
| Padded copy for SIMD | n/a | `padded_string(body_str)` → one copy |
| Scalar keys | `std::string` alloc per key | `string_view` into padded, no alloc |
| String value inside skipped element | **10 MB `std::string` allocated by lexer** | nothing — never lexed |
| Captured element copy | `sliceBuffer()` → `std::string` (one copy) | `std::string(raw_json())` (one copy) |

simdjson trades the zero-copy slab iteration for one extra up-front copy of the
full body (slab chain → contiguous `body_str`), but eliminates the transient
per-string-value allocations that nlohmann's lexer made unavoidable.

For a typical 200 KB chat body with no images, the net difference is small.
For vision requests with large base64 payloads, simdjson eliminates O(image_size)
transient allocations that would otherwise spike RSS on every request.

### Tier summary

| Tier | Condition | Padded buffer | Element copies | Peak live |
|------|-----------|---------------|----------------|-----------|
| 1 | body ≤ `max_element_capture_bytes` | body + padding | Σ element sizes → store | ≈ 2.1× body |
| 2 | body ≤ `max_body_bytes` | body + padding | none | ≈ 2.1× body (no element copies) |
| 3 | body > `max_body_bytes` | never created | n/a | ≤ `max_body_bytes` |

The 2.1× factor in Tiers 1 and 2 is `body_str` (1×) + `padded` (~1× + 64 B).
Both are stack-frame locals freed when `finish()` returns. The long-lived
allocations are scalar strings in `AiRequest` (kilobytes at most) and element
`PayloadRef`s in the store.

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

| Dimension | nlohmann SAX | simdjson on-demand |
|---|---|---|
| **Up-front contiguous copy** | None — `BufferByteIterator` walks slab chain in place | Always — `body_buffer_.toString()` + `padded_string` (body + 64 B) |
| **Peak bytes during `finish()` — no large strings** | ~1× body | ~2.1× body (`body_str` + `padded`) |
| **Peak bytes during `finish()` — 10 MB base64 image, Tier 2** | ~1× body + up to 10 MB transient per string callback | ~2.1× body, flat — no per-string spike |
| **Peak bytes during `finish()` — 10 MB image, Tier 1 capture** | ~1× body + 10 MB transient + 10 MB `sliceBuffer` copy | ~2.1× body + 10 MB `std::string(raw)` copy |
| **Allocations after `finish()` returns** | Scalar strings + element copies | Scalar strings + element copies (identical) |
| **Per-key allocation** | `std::string` per key, always | `string_view` into `padded` — zero alloc |
| **Per-string-value allocation** | `std::string` always, inside lexer, before handler fires — cannot suppress | `string_view` if accessed; nothing if skipped |
| **Skipping a 10 MB base64 string value** | 10 MB `std::string` allocated and immediately destroyed — unavoidable | 0 bytes — cursor advances past it |
| **Skipping entire `messages` array (Tier 2)** | All SAX events fire through every byte; transient strings inside still allocated | `continue` before `field.value()` — zero bytes lexed, zero allocated |
| **Capturing a large element (Tier 1)** | `sliceBuffer()` walks `RawSliceVector`, one exact-sized `reserve` + N `memcpy` (one per slab) | `raw_json()` returns `string_view`, then one `std::string(raw)` copy — equivalent cost |
| **Captured bytes fidelity** | Bit-for-bit identical to request body | Bit-for-bit identical to request body |
| **Key comparison** | `std::string` heap allocation per key, then compare | `string_view` compare against literal — no allocation |
| **Type peeking (`stop`: string vs array)** | Separate `string()` and `start_array()` callbacks; handler state machine | `val.type()` peeks leading byte without consuming — one-liner |
| **Dual-type `id` (string vs int, JSON-RPC)** | Separate `string()` and `number_integer()` callbacks; state flags needed | `val.type()` dispatch, then `get_string()` or `get_int64()` |
| **Agent params extraction** | One-pass: byte-range slice during SAX, then one `json::parse()` in `populateParams()` | Two-pass: `raw_json()` on params in outer parse, then fresh `ondemand::parser` in `populateParams()` |
| **Code complexity** | High — depth counters, `capturing_element_` flags, `elem_start_` tracking, slab arithmetic in `sliceBuffer` | Low — `for (auto field : obj)` with `if/else if` key dispatch; no depth tracking |
| **SAX handler state** | ~10 member variables (`depth_`, `in_messages_`, `in_tools_`, `capturing_element_`, `elem_depth_`, `elem_start_`, `parser_pos_`, `slices_`, …) | None — all state implicit in on-demand cursor |
| **Parse still runs in Tier 2** | Yes — SAX fires through all bytes; transient allocs still happen inside skipped elements | Yes — field iteration runs; skipped arrays cost nothing |
| **SIMD requirement** | None | Requires contiguous padded buffer; SIMD overreads 64 B past end |
| **ARM NEON `-Wold-style-cast`** | No issue | C-style casts in `arm_neon.h` macros require pragma guard around `#include "simdjson.h"` |
| **`ondemand::parser` overhead** | n/a | ~2 KB pre-allocated string builder created fresh each `finish()` — negligible |
| **Error granularity** | Parse abort on any structural error (SAX returns `false`) | Structural error at `parser.iterate()`; field-level errors are soft (`continue`) |
| **Dependency** | `@nlohmann_json` — already in Envoy tree | `@simdjson` — new; single-header amalgamation, same pattern as `simdutf` |
| **Small body (< 1 KB), no large strings** | Slightly better — no extra copy, transient strings tiny | Slightly worse — `padded_string` adds 64 B padding and extra `memcpy` for no benefit |
| **Large body with many large string values (vision, embeddings)** | Significantly worse — O(Σ string sizes) transient allocations unavoidable | Significantly better — transient allocations O(1) regardless of string sizes |
