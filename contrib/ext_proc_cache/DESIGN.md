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
    │
CacheLookupCoordinator (shared, coalescing + deadline-aware retry)
    │
    ├─ distributes CacheEntryMetadata + per-waiter CacheBodyReader
    │  via CacheBodyReaderFactory
    │
CacheStore (pluggable backend, coroutine-native)
    │
    ├─ lookup() returns CacheEntryMetadata + CacheBodyReaderFactory
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
  virtual std::shared_ptr<CacheBodyReaderFactory>
      createBodyReaderFactory(std::string body) = 0;
};
```

`lookup()` returns `CacheLookupResult` containing `CacheEntryMetadata` (headers,
status, timestamps, content_length) and a `CacheBodyReaderFactory`. The factory
produces independent `CacheBodyReader` instances so multiple coalesced waiters
can each stream the body at their own pace.

`createBodyReaderFactory()` is called by the coordinator after a fill to produce
a factory from the filler's accumulated body, without coupling the handler to a
specific store implementation.

Default: `InMemoryCacheStore` — thread-safe LRU with mutex. Body stored as
`shared_ptr<const string>`; readers wrap it with independent offsets.

### Streaming Body Access (`cache_types.h`)

```cpp
class CacheBodyReader {
  virtual Awaitable<std::string> nextChunk(size_t max_size) = 0;
  virtual Awaitable<std::string> readAll() = 0;
};

class CacheBodyReaderFactory {
  virtual std::unique_ptr<CacheBodyReader> createReader() = 0;
};
```

`CacheBodyReader` provides chunk-by-chunk access to cached body data. Each
reader has its own read position and is used by a single consumer. The interface
supports future disk/SSD-backed stores that should not load entire responses into
memory.

- `nextChunk(max_size)` returns the next chunk up to `max_size` bytes; empty
  string when exhausted.
- `readAll()` reads all remaining data into one string. Used for
  `ImmediateResponse` paths (304, 5xx retry) that need the full body.

`CacheBodyReaderFactory` produces independent readers from the same stored body.
For in-memory: wraps `shared_ptr<const string>`, each reader has its own offset.
For disk: each reader would open its own file handle.

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
2. **reportFillSuccess(key, metadata, factory)**: gives each waiter a copy of
   the metadata and an independent `CacheBodyReader` from the factory.
3. **reportFillFailure(key)**: selects next filler by longest remaining
   deadline; if retries exhausted, releases all waiters with `TimedOut`.
4. **cancelWaiter(key, handle)**: removes waiter; if filler, triggers fill
   failure path.

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

Only one body chunk is in memory at a time. For empty bodies, `end_of_stream`
is set on the headers response.

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
  - **Filler with cacheable response**: starts accumulating body via
    `StreamedBodyResponse` (pass-through + buffer for caching).
  - **Filler with 5xx error**: calls `reportFillFailure`, then re-enqueues as
    a waiter via `coordinator_->lookup()`. If the retry filler succeeds, serves
    the cached entry via `ImmediateResponse`; otherwise falls through with the
    error.
  - **Filler with uncacheable response** (e.g. `no-store`): calls
    `reportFillFailure` and passes through the response (valid for the client,
    just not cached).
- **onResponseBody**: passes body through via `StreamedBodyResponse`; if
  storing, buffers body; on `end_of_stream`, stores entry and reports fill
  success with metadata + body reader factory.
- **onResponseTrailers**: if storing, saves trailers, finalizes and stores.
- **onCancel**: reports fill failure if filler.

## File Layout

| File | Contents |
|------|----------|
| `awaitable.h` | `Awaitable<T>` coroutine return type |
| `cache_types.h/.cc` | `CachedEntry`, `CacheEntryMetadata`, `CacheBodyReader`, `CacheBodyReaderFactory`, `CacheLookupResult`, `CoordinatedLookupResult`, `LookupStatus`, cache-control parsing, proto header helpers |
| `cache_store.h` | `CacheStore` abstract interface (lookup returns metadata + reader factory) |
| `in_memory_cache_store.h/.cc` | `InMemoryCacheStore`, `InMemoryBodyReader`, `InMemoryBodyReaderFactory` |
| `cache_key_generator.h` | `CacheKeyGenerator` interface + `DefaultCacheKeyGenerator` |
| `cacheability_checker.h` | `CacheabilityChecker` interface + `DefaultCacheabilityChecker` |
| `cache_age_calculator.h` | `CacheAgeCalculator` interface + `DefaultCacheAgeCalculator` |
| `cache_lookup_coordinator.h/.cc` | `CacheLookupCoordinator` (shared coalescing layer, distributes per-waiter readers) |
| `cache_stream_handler.h/.cc` | `CacheStreamHandler`, `HandleResult` (per-stream ext_proc logic) |
| `server.h/.cc` | Reactor/service with `FULL_DUPLEX_STREAMED` validation and lazy body streaming loop |
| `main.cc` | Wires default implementations |
| `ext_proc_cache_integration_test.cc` | End-to-end tests (miss/hit, coalescing, revalidation, 304, retry, chunked streaming) |
| `BUILD` | Build targets |
