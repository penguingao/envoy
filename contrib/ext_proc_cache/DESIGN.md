# ExtProcCache: HTTP Cache Server Class Hierarchy

## Overview

A standalone ext_proc-based HTTP cache server for Envoy, using C++20 coroutines
with the gRPC `ServerBidiReactor` pattern. Extensible across 4 dimensions:
storage backend, cache key computation, cacheability policy, and cache
age/freshness.

## Architecture

```
ExtProcCacheReactor (per-stream, gRPC coroutine reactor)
    │
    ├─ drives streaming loop: writes headers, then pulls body chunks
    │  one at a time from CacheBodyReader
    │
CacheStreamHandler (per-stream, ext_proc protocol state machine)
    │
    ├─ returns HandleResult { response, body_reader, chunk_size }
    ├─ calls storeHeaders / appendBody / finishBody on the store
    │
CacheLookupCoordinator (shared, coalescing + deadline-aware retry)
    │
    ├─ on cache hit: returns metadata + body reader from store
    ├─ on early release: reportHeadersAvailable() gives each waiter
    │  a tailing reader from the store
    │
CacheStore (pluggable backend, coroutine-native)
    │
    ├─ lookup() returns metadata + tailing or complete body reader
    ├─ storeHeaders() / appendBody() / finishBody() for incremental writes
    └─ store() for atomic writes (304 revalidation)
```

Synchronous extension points (injected into CacheStreamHandler):
- **CacheKeyGenerator** — computes cache key from request headers
- **CacheabilityChecker** — request/response cacheability decisions
- **CacheAgeCalculator** — freshness/age/usability of cached entries

All interfaces operate on **proto HeaderMap**
(`envoy::config::core::v3::HeaderMap`), not Envoy's internal
`Http::HeaderMap`.

## Extension Points

### CacheStore (`cache_store.h`) — coroutine-native

```cpp
class CacheStore {
  // Lookup: returns tailing reader for in-progress entries.
  virtual Awaitable<std::optional<CacheLookupResult>> lookup(const std::string& key) = 0;

  // Atomic store (used for 304 revalidation).
  virtual Awaitable<bool> store(const std::string& key, CachedEntry entry) = 0;
  virtual Awaitable<bool> remove(const std::string& key) = 0;

  // Incremental body write API.
  virtual Awaitable<bool> storeHeaders(const std::string& key, CacheEntryMetadata metadata) = 0;
  virtual void appendBody(const std::string& key, const std::string& data) = 0;
  virtual void finishBody(const std::string& key,
                          std::optional<ProtoHeaderMap> trailers = std::nullopt) = 0;
  virtual void abortBody(const std::string& key) = 0;
};
```

The store is the coordination layer for body streaming. `storeHeaders()` creates
an entry in "filling" state. `appendBody()` appends data and resumes any tailing
readers. `finishBody()` marks the entry complete. `abortBody()` removes the
entry and signals error to readers.

`lookup()` returns a `CacheLookupResult` with `CacheEntryMetadata` and a
`CacheBodyReader`. For complete entries, the reader returns data immediately.
For in-progress entries, the reader tails the writer — suspending when caught up
and resuming when `appendBody` or `finishBody` is called. This means waiters
read at their own pace and late arrivals pick up wherever the writer is.

Default: `InMemoryCacheStore` — thread-safe LRU. Each `InMemoryStoredEntry`
has a per-entry mutex, growing body string, `EntryState` (Filling/Complete/
Failed), and a list of suspended reader coroutine handles.

### Streaming Body Access (`cache_types.h`)

```cpp
class CacheBodyReader {
  virtual Awaitable<std::string> nextChunk(size_t max_size) = 0;
  virtual Awaitable<std::string> readAll() = 0;
};
```

- `nextChunk(max_size)` — returns up to `max_size` bytes; empty when exhausted.
  For tailing readers, suspends when caught up and resumes on new data.
- `readAll()` — reads all remaining data. Used for `ImmediateResponse` paths.

Implementations:
- **`StringBodyReader`** (`cache_types.h`) — reads from a `shared_ptr<const
  string>`. Used by the coordinator when distributing completed bodies to
  very-late-arriving waiters.
- **`InMemoryTailingBodyReader`** (`in_memory_cache_store.h`) — reads from an
  `InMemoryStoredEntry`. Suspends at the write frontier for entries in Filling
  state; reads without suspension for Complete entries.

### CacheKeyGenerator (`cache_key_generator.h`) — sync

```cpp
class CacheKeyGenerator {
  virtual std::string generateKey(const ProtoHeaderMap& request_headers) = 0;
};
```

Default: `DefaultCacheKeyGenerator` — `"{scheme}://{host}{path}"`.

### CacheabilityChecker (`cacheability_checker.h`) — sync

```cpp
class CacheabilityChecker {
  virtual RequestCacheability requestCacheability(const ProtoHeaderMap& request_headers) = 0;
  virtual ResponseCacheability responseCacheability(
      const ProtoHeaderMap& request_headers, const ProtoHeaderMap& response_headers) = 0;
};
```

Default: `DefaultCacheabilityChecker` — RFC 7234 rules (GET only, 200/301/308,
respects `no-store`/`private`/`Authorization`).

### CacheAgeCalculator (`cache_age_calculator.h`) — sync

```cpp
class CacheAgeCalculator {
  virtual CacheEntryUsability calculateUsability(
      const ProtoHeaderMap& request_headers,
      const ProtoHeaderMap& cached_response_headers,
      uint64_t content_length, SystemTime response_time, SystemTime now) = 0;
};
```

Default: `DefaultCacheAgeCalculator` — RFC 7234 freshness lifetime from
`max-age`/`s-maxage`/`Expires`, age calculation, request
`min-fresh`/`max-stale`/`max-age` directives.

## Request Coalescing

`CacheLookupCoordinator` prevents thundering herd by funneling concurrent
lookups for the same key through a single cache fill:

1. **lookup(key, deadline)**: checks store → hit returns metadata + body reader
   (tailing for in-progress entries); miss with no pending fill designates
   caller as filler (`YouFill`); miss with fill in progress suspends the
   coroutine.
2. **reportHeadersAvailable(key)**: called by the filler when cacheable response
   headers are committed to the store via `storeHeaders()`. Releases all
   waiters — each gets a tailing reader from the store via `store->lookup()`.
   Late arrivals also get tailing readers (checked in `SuspendAwaitable`).
3. **reportFillSuccess(key)**: cleans up the pending entry. Releases any
   very-late-arriving waiters with completed readers from the store.
4. **reportFillFailure(key)**: if waiters were already released (streaming from
   store), calls `store->abortBody()` to signal error to tailing readers. If
   not yet released, selects next filler by longest remaining deadline; if
   retries exhausted, releases all waiters with `TimedOut`.
5. **cancelWaiter(key, handle)**: removes waiter; if filler, triggers fill
   failure path.

### Early Release Flow

```
Time →
Filler:   [req_headers] → [resp_headers] → [body₁] → [body₂] → [eos]
                               │               │         │         │
                         storeHeaders()   appendBody  appendBody  finishBody
                               │
                     reportHeadersAvailable
                               │
Waiter A: [suspended]──→ [store.lookup: tailing reader] → [body₁] → [body₂] → [eos]
Waiter B: [suspended]──→ [store.lookup: tailing reader] → [body₁] → [body₂] → [eos]

Late waiter C arrives during body₂:
Waiter C: ──────────────→ [store.lookup: tailing reader] → ──────── [body₂] → [eos]
                           (reads body₁ immediately from store buffer,
                            catches up, tails from body₂ onward)
```

Each waiter reads from the store at its own pace. The store's tailing reader
suspends when caught up and resumes when `appendBody` is called. Late arrivals
get a reader that starts from offset 0 and reads all buffered data before
tailing.

## Streaming Mode

The server requires **`FULL_DUPLEX_STREAMED`** response body mode. On the first
`ProcessingRequest`, the server validates `protocol_config.response_body_mode`
and rejects the stream with `INVALID_ARGUMENT` if it is not
`FULL_DUPLEX_STREAMED`. Body chunks are passed through using
`StreamedBodyResponse` in the `BodyMutation`, and trailer modes must be set to
`SEND`.

## Cache Hit Delivery

Cache hits from `request_headers` are served via `StreamedImmediateResponse`
with a configurable `chunk_size` (default 64KB). The handler builds the headers
response and passes the `CacheBodyReader` to the reactor via `HandleResult`.
The reactor drives the streaming loop:

1. Write `StreamedImmediateResponse` with `headers_response` (`:status` +
   cached headers + optional `age`).
2. Read chunks lazily: `co_await body_reader->nextChunk(chunk_size)`.
3. Write each chunk as `StreamedImmediateResponse` with `body_response`.
4. Use one-chunk-ahead reading to set `end_of_stream` on the last chunk.

Only one body chunk is in memory at a time. For tailing readers,
`nextChunk()` suspends when the reader catches up to the filler, resuming
when more data arrives.

Cache hits from `response_headers` (304 revalidation, 5xx retry) use
`ImmediateResponse` since `StreamedImmediateResponse` is only valid in response
to `request_headers`. These paths call `reader->readAll()` to materialize the
body.

## ext_proc Flow

- **onRequestHeaders**: generate key → check request cacheability → coordinated
  lookup → `Hit` serves via `StreamedImmediateResponse` (headers + lazy body
  chunks from store reader); stale `Hit` (requires validation) injects
  conditional headers (`If-None-Match`/`If-Modified-Since`) and forwards to
  upstream; `YouFill` continues to upstream; `TimedOut`/`Cancelled` continues
  without caching.
- **onResponseHeaders**:
  - **304 during validation**: refreshes cached headers per RFC 7234 §4.3.4,
    re-stores the entry atomically, and serves the cached body via
    `ImmediateResponse`.
  - **Filler with cacheable response**: calls `storeHeaders()` to commit
    metadata to the store, then `reportHeadersAvailable()` to release waiters
    with tailing readers.
  - **Filler with 5xx error**: calls `reportFillFailure`, then re-enqueues as
    a waiter via `coordinator_->lookup()`. If the retry filler succeeds, serves
    the cached entry via `ImmediateResponse`; otherwise falls through with the
    error.
  - **Filler with uncacheable response** (e.g. `no-store`): calls
    `reportFillFailure` and passes through the response.
- **onResponseBody**: passes body through via `StreamedBodyResponse`; if
  storing, calls `appendBody()` on the store (tailing readers resume); on
  `end_of_stream`, calls `finishBody()` and `reportFillSuccess()`.
- **onResponseTrailers**: if storing, calls `finishBody()` with trailers and
  `reportFillSuccess()`.
- **onCancel**: if storing, calls `abortBody()` to signal error to tailing
  readers; calls `reportFillFailure()` if filler.

## Future Considerations

### Decoupling Fill Lifetime from ext_proc Stream

Currently, if the filler's downstream client resets the HTTP request, Envoy
cancels the ext_proc stream, which triggers `onCancel` and abandons the cache
fill mid-stream. Tailing readers in the store receive an error signal via
`abortBody()`. Waiters not yet released fall back to the retry path.

This is a limitation of the ext_proc architecture: the upstream response data
flows through the ext_proc filter, so when the filter's stream dies, the data
source dies. The ext_proc server cannot unilaterally keep the fill alive.

Possible solutions (requiring Envoy filter changes):
- **Filter-level fill detach**: configure the ext_proc filter to keep the
  upstream connection alive and forward body data even after the downstream
  client disconnects, if the ext_proc server indicates the fill should continue.
- **Server-side upstream fetch**: the ext_proc server fetches directly from the
  upstream (bypassing Envoy's ext_proc filter for the fill), so the fill is
  independent of any client stream. This fundamentally changes the architecture.

For now, the server relies on fast retry: the coordinator promotes the next
waiter to filler quickly so a re-fetch starts immediately.

## File Layout

| File | Contents |
|------|----------|
| `awaitable.h` | `Awaitable<T>` coroutine return type with `syncGet()` |
| `cache_types.h/.cc` | `CachedEntry`, `CacheEntryMetadata`, `CacheBodyReader`, `StringBodyReader`, `CacheLookupResult`, `CoordinatedLookupResult`, `LookupStatus`, cache-control parsing, proto header helpers |
| `cache_store.h` | `CacheStore` abstract interface (lookup + incremental write API) |
| `in_memory_cache_store.h/.cc` | `InMemoryCacheStore`, `InMemoryStoredEntry`, `InMemoryTailingBodyReader` (thread-safe LRU with per-entry tailing reader support) |
| `cache_key_generator.h` | `CacheKeyGenerator` interface + `DefaultCacheKeyGenerator` |
| `cacheability_checker.h` | `CacheabilityChecker` interface + `DefaultCacheabilityChecker` |
| `cache_age_calculator.h` | `CacheAgeCalculator` interface + `DefaultCacheAgeCalculator` |
| `cache_lookup_coordinator.h/.cc` | `CacheLookupCoordinator` (coalescing, early release via store, retry) |
| `cache_stream_handler.h/.cc` | `CacheStreamHandler`, `HandleResult` (per-stream ext_proc logic) |
| `server.h/.cc` | Reactor/service with `FULL_DUPLEX_STREAMED` validation and lazy body streaming loop |
| `main.cc` | Wires default implementations |
| `ext_proc_cache_integration_test.cc` | End-to-end tests (miss/hit, coalescing, revalidation, 304, retry, chunked streaming) |
| `BUILD` | Build targets |
