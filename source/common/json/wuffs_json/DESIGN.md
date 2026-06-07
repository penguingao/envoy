# WuffsJsonCursor — Design Document

## Overview

`WuffsJsonCursor` is a streaming, SAX-style JSON parser built on the
[Wuffs](https://github.com/google/wuffs) library. It tokenizes a JSON document
delivered as a sequence of byte chunks and fires synchronous callbacks into a
`Handler` as semantic events are recognized. All low-level Wuffs mechanics —
token ring-buffer management, coroutine suspension, escape-sequence decoding —
are hidden behind the Handler interface; consumers see only clean, decoded
events with no knowledge of the underlying tokenizer.

---

## Motivation

The AI proxy ingests LLM inference requests (OpenAI, Anthropic, etc.) that
arrive as HTTP body chunks on Envoy's event loop. Parsing these requests has
two conflicting requirements:

1. **Selective extraction**: Only a handful of fields — `model`, `stream`,
   `stop[]`, sampling parameters — are needed for routing and protocol
   translation. The bulk of the payload (message content, tool call results)
   must pass through untouched, often unread at the byte level.

2. **Zero head-of-line blocking**: The full body must arrive before a routing
   decision can be made (JSON is not required to be in key-order, so `model`
   might appear after `messages`). We cannot block the event loop waiting for
   `mmap` page faults on large content fields.

A general-purpose DOM parser (nlohmann/json, rapidjson) fails requirement 1: it
allocates a full in-memory tree for every field including content we don't need.
A bespoke hand-rolled parser fails maintainability. `WuffsJsonCursor` threads
the needle: single-pass token-driven, with per-string routing decisions that
short-circuit all allocation and copy for uninteresting fields.

---

## Why Wuffs

Wuffs (Wrangling Unsafe Functionality Safely) is a memory-safe,
single-header C library that compiles with a verified-correct tokenizer for
JSON. Key properties relevant here:

- **Single-header, no linking**: `wuffs-v0.4.c` is committed directly; one
  `#include` in `wuffs_impl.c` (with `WUFFS_IMPLEMENTATION`) activates it.
- **Stackless coroutine**: The decoder preserves its full parse state across
  calls. When input is exhausted mid-string, Wuffs suspends (`short_read`);
  the next `feed()` call resumes at the exact source byte.
- **Token ring buffer**: Tokens are written into a fixed-size circular array
  (`tok_data_[256]`). No heap allocation per token.
- **Escape decoding built-in**: `\n`, `\t`, `\uXXXX` are emitted as
  `UNICODE_CODE_POINT` tokens with the decoded code point in `vbd`. The
  cursor converts these to UTF-8 with a hand-rolled four-case encoder.

---

## Streaming Model

```
cursor.feed(chunk₁, closed=false)
cursor.feed(chunk₂, closed=false)
...
cursor.feed(chunkₙ, closed=true)
```

The cursor is designed for HTTP body chunks arriving incrementally on an event loop. Each `feed()` call hands the cursor one chunk of bytes. The cursor parses as much as it can from that chunk, fires any Handler callbacks for complete tokens, and then suspends — all state is preserved inside the cursor object. The next `feed()` call resumes exactly where the previous one stopped.

Set `closed=true` on the final chunk to signal end-of-stream. `feed()` returns a non-OK status immediately on malformed JSON; otherwise it returns OK whether or not the document is complete yet.

**Chunk boundaries are transparent.** A chunk may split at any byte position — mid-string, mid-number, even in the middle of a `\uXXXX` escape sequence. The cursor handles this correctly: a string value that straddles three chunks will still produce a single, complete `closeStringCapture` callback once the closing quote arrives.

---

## Token Model

```
for each token in body:

    FILLER              → skip (whitespace, commas, colons)

    STRUCTURE { [       → depth++
                          handler.onContainerOpen(key, is_dict, depth, tok_start)
    STRUCTURE } ]       → handler.onContainerClose(depth, tok_end)
                          depth--

    STRING key          → accumulate chars → handler.onKey(key, depth)
    STRING value open   → str_target = handler.openStringCapture(key, depth, tok_start)
    STRING value chunk  → append bytes to *str_target  (if capturing; else skip)
    STRING value close  → handler.closeStringCapture(str_target, key, depth, tok_end)

    UNICODE_CODE_POINT  → decode escape to UTF-8 → append to *str_target

    NUMBER              → handler.onNumber(key, raw, depth)
    LITERAL true|false  → handler.onBoolean(key, value, depth)
    LITERAL null        → handler.onNull(key, depth)
```

Each `wuffs_base__token` carries three fields:

| Field | Meaning |
|---|---|
| `vbc` (value_base_category) | Coarse token kind; drives the `switch` |
| `vbd` (value_base_detail) | Kind-specific bit flags or decoded value |
| `tlen` (token length) | Source bytes consumed; used to advance `body_src_pos_` |

The `body_src_pos_` counter is advanced by `tlen` on every token, producing a
monotonically increasing global byte offset that is threaded into the `tok_start`
/ `tok_end` parameters of every callback.

### VBC Dispatch Table

| VBC | Example input | Action |
|---|---|---|
| `FILLER` | whitespace, `,`, `:` | Advance `body_src_pos_`; no callback |
| `STRUCTURE` | `{` `}` `[` `]` | Manage `depth_`, `is_dict_[]`, `expecting_key_[]`; fire `onContainerOpen` / `onContainerClose` with byte offsets |
| `STRING` | plain ASCII bytes, quote delimiters | Key strings → `str_acc_`; value strings → `str_target_` if non-null; `DROP` bits skip quote chars, `COPY` bits append bytes |
| `UNICODE_CODE_POINT` | `\n`, `\t`, `\uXXXX` | Decode to UTF-8; append to `*str_target_` if non-null; else zero-cost skip |
| `NUMBER` | `42`, `1.5`, `-3e10` | Forward raw bytes to `onNumber(key, raw, depth)` |
| `LITERAL` | `true`, `false`, `null` | Dispatch to `onBoolean` or `onNull` |

**Key subtlety**: backslash escapes *never* arrive as `STRING` tokens. Wuffs
always emits them as `UNICODE_CODE_POINT` tokens. This means the `STRING` case
handles only plain ASCII bytes and quote delimiters; the `UNICODE_CODE_POINT`
case handles all non-ASCII content including `\n` and `\uXXXX`. Both cases
gate on `str_target_` so the routing decision made in `openStringCapture`
applies uniformly.

---

## Handler Interface

The `Handler` abstract class defines the eight callbacks the cursor fires.

### Depth and Key Model

- `depth` starts at 0 before the root container.
- `onContainerOpen` fires *after* depth is incremented; `onContainerClose` fires *before* decrement. Both report the depth of the container.
- Inside a dict, callbacks receive `key` — the dict key immediately to the left
  of the value. For array elements, `key` is `""`.

```
{ "a": { "b": [ 1, 2 ] } }
 ^d=1    ^d=2   ^d=3
```

Example callback sequence for `{"messages": [{"role": "user"}]}`:

```
onContainerOpen    (key="",         is_dict=true,  depth=1)   ← root {
onKey              ("messages",                    depth=1)
onContainerOpen    (key="messages", is_dict=false, depth=2)   ← [
onContainerOpen    (key="",         is_dict=true,  depth=3)   ← { (parent is array)
onKey              ("role",                        depth=3)
openStringCapture  ("role",         depth=3, tok_start)  → &buf or nullptr
closeStringCapture (&buf, "role",   depth=3, tok_end)
onContainerClose   (depth=3, tok_end)
onContainerClose   (depth=2, tok_end)
onContainerClose   (depth=1, tok_end)
```

### String Value Lifecycle: `openStringCapture` / `closeStringCapture`

These two callbacks implement the core routing mechanism for string values.

```
openStringCapture(key, depth, tok_start) → std::string* | nullptr
closeStringCapture(target, key, depth, tok_end)
```

`openStringCapture` is called once per string value at the opening `"`. The
handler inspects `key` and `depth` and returns either:

- **A handler-owned `std::string*`**: the cursor sets `str_target_` to this
  pointer and appends decoded UTF-8 into it across all subsequent `STRING` and
  `UNICODE_CODE_POINT` tokens for this string — across however many `feed()`
  calls it spans. When the closing `"` is seen (`cont=false`), `closeStringCapture`
  fires with the same pointer and the buffer holds the complete value.

- **`nullptr`**: the cursor sets `str_target_ = nullptr`. Every subsequent
  `STRING` and `UNICODE_CODE_POINT` token for this value hits the write guard
  `if (str_target_)` → false → skipped. No bytes are written, no allocation
  occurs, and `closeStringCapture` is never called. The cost is a single
  branch predicting false, regardless of how large the string is.

#### Decoded UTF-8, Not Raw JSON Bytes

The buffer receives **fully decoded UTF-8**. Wuffs never emits backslash
escapes in `STRING` tokens; the cursor converts `UNICODE_CODE_POINT` tokens to
UTF-8 before appending. This is correct for semantic use cases: routing
decisions, keyword matching, logging. If you need to re-serialize a captured
value back to JSON, you must re-escape it. For verbatim forwarding of large
values (e.g., message content), use byte ranges from
`onContainerOpen`/`onContainerClose` instead — those capture raw JSON bytes
without any decoding.

#### Why This Design (vs. `onStringChunk`)

An alternative design fires one callback per Wuffs token, regardless of
interest:

```cpp
// onStringChunk alternative — every chunk of every string fires
absl::Status onStringChunk(key, depth, chunk, end, tok_start);
```

This is strictly worse:

| Criterion | `openStringCapture` (chosen) | `onStringChunk` (alternative) |
|---|---|---|
| Accumulation ownership | Cursor (once, shared) | Every handler must reimplement |
| Discard cost | Zero — `nullptr` skips all subsequent work | Handler receives every chunk and must check + ignore |
| Key dispatch cost | Once per string | Once per chunk |
| Large unwanted string (e.g. 10 KB content) | Zero overhead | ~40 callbacks, each discarded |
| State machine complexity | None | Handler must track `accumulating_` boolean, handle single-chunk edge case |

The `std::string*` return type looks unusual but is intentional: the handler
provides the storage, the cursor writes to it, and `nullptr` means "not
interested." One clear comment resolves the readability concern; the
accumulation burden of the alternative is a design problem that every consumer
would independently get wrong.

### Container Byte Ranges

```cpp
onContainerOpen(key, is_dict, depth, tok_start)
onContainerClose(depth, tok_end)
```

`tok_start` is the byte offset of the opening `{` or `[` in the global body
stream. `tok_end` is the byte offset immediately past the closing `}` or `]`.
Together they form a half-open range `[tok_start, tok_end)` that identifies a
sub-region of the original body buffer — useful for zero-copy extraction when
the body is memory-mapped.

The same principle applies to strings: `openStringCapture` receives `tok_start`
at the opening `"` and `closeStringCapture` receives `tok_end` just past the
closing `"`, so string values also support raw byte-range extraction.

### Scalar Callbacks

```cpp
onNumber(key, raw, depth)   // raw bytes; parse with absl::SimpleAtoi/SimpleAtod
onBoolean(key, value, depth)
onNull(key, depth)
onKey(key, depth)           // dict key completion; return non-OK to abort
```

`onNumber` and `onBoolean` return `absl::Status` to allow abort on parse
error (e.g. out-of-range value). `onKey` returns `absl::Status` for
duplicate-key detection. Keys exceeding 256 bytes are rejected with
`InvalidArgumentError`.

---

## Path Tracking

Construct with `track_paths=true` and call these from within any callback:

```cpp
buildIndexedPath(depth)   // → "messages[0].role"   concrete array index
buildPatternPath(depth)   // → "messages[].role"    wildcard index
```

### Why key + depth alone is not enough

A Handler callback receives `key` and `depth`, but those two values do not
uniquely identify a position in the tree. Consider:

```json
{
  "messages": [ { "content": "hello" } ],
  "tools":    [ { "function": { "content": "do something" } } ]
}
```

Both `"content"` fields arrive with `key="content"` and `depth=3`.  The
callback cannot tell which structural location it is at — user message text
that should be forwarded, or tool metadata that should be ignored.

`buildPatternPath(3)` resolves the ambiguity:

```
messages[].content        ← first occurrence
tools[].function.content  ← second occurrence
```

The cursor tracks the full ancestor chain — which keys opened which containers
at each depth — and reconstructs the complete structural path on demand.
"Ambiguous position" means any location that is uniquely identified only by its
full ancestor chain, not by its immediate `key` and `depth` alone.

**`buildPatternPath`** is called in `openStringCapture` for config-driven
extraction. Patterns use `[]` as an index wildcard, so the rule
`messages[].content` matches every array element regardless of its position.
The handler checks the pattern path against a configured pattern set.

**`buildIndexedPath`** is called when a concrete element identity is needed —
for example, to key extracted attributes to the element they came from
(`messages[2].role` vs `messages[3].role`) or for logging.

Both methods are O(depth) string concatenation, only called at string routing
time (once per string value, not once per token).

---

## Integration with MmapPayloadStore

`WuffsJsonCursor` is used inside `InferenceBodyParser`, which implements
`Handler` and writes every incoming chunk to a `residual_writer_` that
appends into a memory-mapped file (anonymous `mkstemp`-unlinked temp file).

### Dual-Write per Chunk

```cpp
// InferenceBodyParser::feed()
residual_writer_.append(chunk);    // memcpy into mmap region (raw bytes preserved)
cursor_.feed(chunk, closed);       // parse same bytes, fire callbacks
```

Both happen on the same chunk. The cursor reads from the chunk for parsing;
`residual_writer_` stores those bytes verbatim in the mmap file for later
forwarding.

### Message / Tool Byte Ranges

Inside `onContainerOpen` / `onContainerClose`, the parser records element
boundaries into `message_ranges_` and `tool_ranges_` as pairs of `(tok_start,
tok_end)` offsets into the mmap region — no content copy.

```cpp
onContainerOpen(...)  → elem_start_ = tok_start
onContainerClose(...) → message_ranges_.push({elem_start_, tok_end})
```

At finish time, `makeSubRef(message_ranges_)` converts each offset pair into a
`PayloadRef::External { offset, length }` — just two integers pointing into the
mmap file. The content text is never copied.

### PayloadRef Storage Types

| Type | Storage | When used |
|---|---|---|
| `Inline` | Embedded in `PayloadRef` itself (≤ 4 KB) | Short scalars: `model`, `stream`, temperature |
| `External` | `{offset, length}` into mmap | Large fields: messages[], tools[], residual body |
| `Buffered` | Heap `OwnedImpl` after prefetch | After async prefetch upgrades an External ref |

### Async Prefetch

Before dispatch, all `External` refs are prefetched off the event loop:
`pread(fd, buf, offset, len)` on background threads materializes mmap pages
without blocking. Page faults happen here, not on the event loop. An atomic
countdown triggers `doDispatch()` when all refs are `Buffered`.

### Copy Budget for Large Content

A 10 KB message content field crosses memory **three times**:

1. **Network buffer → mmap**: `feed()` → `residual_writer_.append()` → memcpy.
   Required because Envoy recycles network buffers after `decodeData()` returns.

2. **mmap → heap `OwnedImpl`**: async prefetch runs `pread(fd, buf, offset, len)`
   on a background thread, upgrading the `External` ref to `Buffered`.
   Required to materialize mmap pages off the event loop — page faults happen
   here, not during dispatch.

3. **heap → upstream send buffer**: `ref.toString()` inside re-assembly →
   `body.dump()` → `addDecodedData()`.
   Required because the upstream connection needs its own writable buffer.

Everything between these copies — the cursor parsing, `message_ranges_`
recording, `PayloadRef` passing — touches **zero bytes** of content. Only
`{offset, length}` integers move. The cursor itself never allocates for or
copies unselected strings.

---

## Re-Assembly Path

After parsing and prefetch, `RequestEncoder` / `AnthropicRequestEncoder`
re-encodes the request:

1. Parse `residual_params` (top-level fields the cursor didn't extract) from the
   mmap-backed `PayloadRef` via `json::parse(payload->residual_params.toString())`.
2. Overlay captured scalars: `body["model"] = payload->target.name`, `body["stream"] = ...`.
3. Splice `messages[]` and `tools[]` back in from their `Buffered` refs via
   `ref.toString()` — verbatim raw JSON bytes, no re-escaping.
4. `body.dump()` → `addDecodedData()` → `continueDecoding()` → upstream.

The original request body is consumed and discarded. The parser builds an
`AiRequest` struct; the encoder re-serializes it, potentially translating
format (e.g. OpenAI → Anthropic).


---

## Depth and Key Limits

| Limit | Value | Rationale |
|---|---|---|
| `kMaxDepth` | 8 | Covers all practical LLM inference request schemas |
| `kMaxKeyBytes` | 256 | Protects against malformed inputs; real keys are far shorter |
| Token ring size | 256 slots | Balance between batch efficiency and stack size |

Fields or containers beyond depth 8 are silently passed through (callbacks
fire with clamped depth state). Keys beyond 256 bytes cause `feed()` to return
`InvalidArgumentError`.

---

## Duplicate Key Prevention

The JSON specification permits duplicate keys but leaves their semantics
undefined. In an AI proxy this ambiguity is a security risk: an attacker can
craft a request where the proxy validates one occurrence of a key (`"model":
"safe"`) while the upstream LLM receives a different one (`"model":
"dangerous"`), bypassing any routing or authorization check.

### How the cursor prevents it

`WuffsJsonCursor` maintains one `absl::flat_hash_set<std::string>` per depth
level (`seen_keys_[kMaxDepth]`). On every container open the set for that depth
is cleared, giving each new object its own key namespace. When a dict key
completes, the cursor attempts to insert it into `seen_keys_[depth_]`. If the
insert returns false (key already present), `feed()` returns
`InvalidArgumentError` immediately — before `onKey` is called, before any value
bytes are processed, and before any handler state is updated.

### Why the cursor, not the handler

Two designs are possible:

**Handler-based detection** — the handler tracks seen keys in its own `onKey`
implementation and returns a non-OK `absl::Status` to abort.

**Cursor-based detection** — the cursor rejects before calling `onKey` at all.

| | Cursor | Handler |
|---|---|---|
| Protection scope | Every handler automatically | Only handlers that implement it |
| New handler risk | Zero — protection is unconditional | Easy to forget under time pressure |
| Memory | `flat_hash_set` per depth in cursor | Same structure duplicated per handler |
| Policy flexibility | Fixed: reject on duplicate | Handler can choose last-write-wins etc. |

The cursor approach is correct here because duplicate key rejection is a
**structural property of the input**, not a semantic policy. It is analogous to
rejecting malformed JSON, which the cursor already owns. Making it a handler
convention means every present and future handler must independently implement
it — one omission is a vulnerability. The cursor is the only component that
sees every key in sequence and can enforce the invariant unconditionally.

### Data structure choice

`absl::flat_hash_set<std::string>` is more memory-efficient than
`std::set<std::string>`:

- `std::set` allocates one tree node per key (separate `malloc`, plus
  left/right/parent pointers — ~40 bytes overhead per entry).
- `flat_hash_set` uses open addressing: one contiguous backing allocation for
  all entries, no per-node pointers. For short keys (model, temperature, role)
  SSO applies so the strings themselves do not heap-allocate either.

The set is cleared (not destroyed) on each container open, so the backing
allocation is reused across sibling objects at the same depth.

---

## Testing

Eleven unit tests cover the full token dispatch surface:

| Test | What it verifies |
|---|---|
| `EmptyObject` | Root `{}` fires no field callbacks |
| `FlatStringFields` | `openStringCapture` / `closeStringCapture` round-trip |
| `ScalarFields` | `onNumber`, `onBoolean`, `onNull` for all scalar types |
| `StringEscapes` | `UNICODE_CODE_POINT` path: `\n`, `\t`, `\uXXXX` → decoded UTF-8 |
| `NestedObjectDiscarded` | `openStringCapture` returning `nullptr` at depth > 1 incurs zero allocation |
| `StreamingAcrossChunks` | Wuffs coroutine state persists across `feed()` calls; string value straddling a chunk boundary is reassembled correctly |
| `InvalidJsonReturnsError` | Malformed input returns non-OK `absl::Status` |
| `DuplicateKeyRejected` | Duplicate key in flat object returns error |
| `DuplicateKeyInNestedObjectRejected` | Duplicate key at depth > 1 returns error |
| `SameKeyNameAtDifferentDepthsAllowed` | `{"a":{"a":1}}` — same name at different depths is not a duplicate |
| `SameKeyNameInSiblingObjectsAllowed` | `[{"a":1},{"a":2}]` — same name in sibling objects is not a duplicate |

---

## Build Integration

Wuffs is fetched as a Bazel external dependency:

```python
# bazel/repository_locations.bzl
wuffs = {
    "version": "0.4.0-alpha.9",
    "sha256": "...",
    "urls": ["https://github.com/google/wuffs/archive/..."],
}
```

The single-header C file is activated via a one-line translation unit:

```c
// wuffs_impl.c
#define WUFFS_IMPLEMENTATION
#include "release/c/wuffs-v0.4.c"
```

The `wuffs_json.h` header includes `wuffs-v0.4.c` without
`WUFFS_IMPLEMENTATION` (declarations only). `wuffs_impl.c` defines the
implementation exactly once.



