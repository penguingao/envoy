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
    │  one at a time from CacheBodyReader (StringBodyReader or FollowingBodyReader)
    │
CacheStreamHandler (per-stream, ext_proc protocol state machine)
    │
    ├─ returns HandleResult { response, body_reader, chunk_size }
    │
CacheLookupCoordinator (shared, coalescing + deadline-aware retry)
    │
    ├─ on cache hit: returns metadata + StringBodyReader
    ├─ on early release: reportHeadersAvailable() gives waiters metadata
    │  + FollowingBodyReader tailing a SharedBodyStream
    │
CacheStore (pluggable backend, coroutine-native)
    │
    ├─ lookup() returns CacheEntryMetadata + CacheBodyReader
    └─ store() accepts CachedEntry (metadata + full body string)
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
  virtual Awaitable<std::optional<CacheLookupResult>> lookup(const std::string& key) = 0;
  virtual Awaitable<bool> store(const std::string& key, CachedEntry entry) = 0;
  virtual Awaitable<bool> remove(const std::string& key) = 0;
};
```

`lookup()` returns `CacheLookupResult` containing `CacheEntryMetadata` (headers,
status, timestamps, content_length) and a `CacheBodyReader`. The reader provides
chunk-by-chunk access to the body.

Default: `InMemoryCacheStore` — thread-safe LRU with mutex. Body stored as
`shared_ptr<const string>`; lookup returns a `StringBodyReader`.

### Streaming Body Access (`cache_types.h`)

```cpp
class CacheBodyReader {
  virtual Awaitable<std::string> nextChunk(size_t max_size) = 0;
  virtual Awaitable<std::string> readAll() = 0;
};
```

Implementations:

- **`StringBodyReader`** — reads from a `shared_ptr<const string>` with an
  independent offset. Used by the in-memory store and the coordinator for
  distributing completed bodies to late waiters.

- **`FollowingBodyReader`** — tails a `SharedBodyStream` being written by a
  filler. Suspends when caught up to the write frontier, resumes when the
  filler appends more data or signals completion. Used for early release of
  coalesced waiters.

### SharedBodyStream (`cache_types.h`)

Shared append-only buffer written by the filler and read by multiple
`FollowingBodyReaders` at independent offsets:

- `append(data)` — appends data, resumes all waiting readers.
- `finish()` — signals end-of-stream, resumes all waiting readers.
- `signalError()` — signals failure, resumes all waiting readers (they
  return empty, terminating the stream).

Synchronization: mutex protects buffer/state; coroutine handles are collected
under the lock and resumed outside it to avoid deadlock. This mirrors the
existing pattern in `reportFillSuccess` where the filler's thread resumes
waiter coroutines that call `StartWrite` (thread-safe in gRPC callback API).

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

1. **lookup(key, deadline)**: checks store → hit returns metadata + body reader;
   miss with no pending fill designates caller as filler (`YouFill`); miss with
   fill in progress suspends the coroutine.
2. **reportHeadersAvailable(key, metadata, stream)**: called by the filler when
   cacheable response headers arrive. Releases all waiters immediately with
   metadata + `FollowingBodyReader` tailing the `SharedBodyStream`. Waiters
   start receiving body chunks in real time as the filler streams them from
   upstream.
3. **reportFillSuccess(key, metadata, body)**: if waiters were already released
   via `reportHeadersAvailable`, just cleans up the pending entry. Otherwise
   (fallback), distributes metadata + `StringBodyReader` to waiters.
4. **reportFillFailure(key)**: if waiters were already released, signals error
   on the `SharedBodyStream` (readers return empty). If not yet released,
   selects next filler by longest remaining deadline; if retries exhausted,
   releases all waiters with `TimedOut`.
5. **cancelWaiter(key, handle)**: removes waiter; if filler, triggers fill
   failure path.

### Early Release Flow

```
Time →
Filler:   [req_headers] → [resp_headers: cacheable!] → [body₁] → [body₂] → [eos] → [store]
                                   │                       │         │         │
                           reportHeadersAvailable     append()  append()  finish()
                                   │
Waiter A: [suspended]────────→ [released: headers] → [body₁] → [body₂] → [eos]
Waiter B: [suspended]────────→ [released: headers] → [body₁] → [body₂] → [eos]
```

Waiters receive headers and start streaming body chunks at the same pace as
the filler. `FollowingBodyReader::nextChunk()` suspends when caught up and
resumes when the filler appends more data.

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

Only one body chunk is in memory at a time. For `FollowingBodyReader`, the
`nextChunk()` call suspends when the reader catches up to the filler, resuming
when more data arrives.

Cache hits from `response_headers` (304 revalidation, 5xx retry) use
`ImmediateResponse` since `StreamedImmediateResponse` is only valid in response
to `request_headers`. These paths call `reader->readAll()` to materialize the
body.

## ext_proc Flow

- **onRequestHeaders**: generate key → check request cacheability → coordinated
  lookup → `Hit` serves via `StreamedImmediateResponse` (headers + lazy body
  chunks); stale `Hit` (requires validation) injects conditional headers
  (`If-None-Match`/`If-Modified-Since`) and forwards to upstream; `YouFill`
  continues to upstream; `TimedOut`/`Cancelled` continues without caching.
- **onResponseHeaders**:
  - **304 during validation**: refreshes cached headers per RFC 7234 §4.3.4,
    stores the updated entry, and serves the cached body via
    `ImmediateResponse`.
  - **Filler with cacheable response**: creates `SharedBodyStream`, calls
    `reportHeadersAvailable` to release waiters immediately, starts
    accumulating body.
  - **Filler with 5xx error**: calls `reportFillFailure`, then re-enqueues as
    a waiter via `coordinator_->lookup()`. If the retry filler succeeds, serves
    the cached entry via `ImmediateResponse`; otherwise falls through with the
    error.
  - **Filler with uncacheable response** (e.g. `no-store`): calls
    `reportFillFailure` and passes through the response (valid for the client,
    just not cached).
- **onResponseBody**: passes body through via `StreamedBodyResponse`; if
  storing, buffers body and pushes to `SharedBodyStream`; on `end_of_stream`,
  finishes the shared stream, stores entry, and reports fill success.
- **onResponseTrailers**: if storing, finishes the shared stream, saves
  trailers, stores entry, and reports fill success.
- **onCancel**: signals error on `SharedBodyStream` if active; reports fill
  failure if filler.

## Future Considerations

### Decoupling Fill Lifetime from ext_proc Stream

Currently, if the filler's downstream client resets the HTTP request, Envoy
cancels the ext_proc stream, which triggers `onCancel` and abandons the cache
fill mid-stream. Coalesced waiters that were already released via
`reportHeadersAvailable` receive a truncated body (error signal on the
`SharedBodyStream`). Waiters not yet released fall back to the retry path.

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
| `awaitable.h` | `Awaitable<T>` coroutine return type |
| `cache_types.h/.cc` | `CachedEntry`, `CacheEntryMetadata`, `CacheBodyReader`, `StringBodyReader`, `SharedBodyStream`, `FollowingBodyReader`, `CacheLookupResult`, `CoordinatedLookupResult`, `LookupStatus`, cache-control parsing, proto header helpers |
| `cache_store.h` | `CacheStore` abstract interface (lookup returns metadata + reader) |
| `in_memory_cache_store.h/.cc` | `InMemoryCacheStore` (thread-safe LRU) |
| `cache_key_generator.h` | `CacheKeyGenerator` interface + `DefaultCacheKeyGenerator` |
| `cacheability_checker.h` | `CacheabilityChecker` interface + `DefaultCacheabilityChecker` |
| `cache_age_calculator.h` | `CacheAgeCalculator` interface + `DefaultCacheAgeCalculator` |
| `cache_lookup_coordinator.h/.cc` | `CacheLookupCoordinator` (coalescing, early release via `reportHeadersAvailable`, retry) |
| `cache_stream_handler.h/.cc` | `CacheStreamHandler`, `HandleResult` (per-stream ext_proc logic) |
| `server.h/.cc` | Reactor/service with `FULL_DUPLEX_STREAMED` validation and lazy body streaming loop |
| `main.cc` | Wires default implementations |
| `ext_proc_cache_integration_test.cc` | End-to-end tests (miss/hit, coalescing, revalidation, 304, retry, chunked streaming) |
| `BUILD` | Build targets |
