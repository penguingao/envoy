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
CacheStreamHandler (per-stream, ext_proc protocol state machine)
    │
CacheLookupCoordinator (shared, coalescing + deadline-aware retry)
    │
CacheStore (pluggable backend, coroutine-native)
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
  virtual Awaitable<std::optional<CachedEntry>> lookup(const std::string& key) = 0;
  virtual Awaitable<bool> store(const std::string& key, CachedEntry entry) = 0;
  virtual Awaitable<bool> remove(const std::string& key) = 0;
};
```

Default: `InMemoryCacheStore` — thread-safe LRU with mutex.

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

1. **lookup(key, deadline)**: checks store → hit returns immediately; miss with
   no pending fill designates caller as filler (`YouFill`); miss with fill in
   progress suspends the coroutine.
2. **reportFillSuccess(key, entry)**: resumes all waiters with `Hit`.
3. **reportFillFailure(key)**: selects next filler by longest remaining
   deadline; if retries exhausted, releases all waiters with `TimedOut`.
4. **cancelWaiter(key, handle)**: removes waiter; if filler, triggers fill
   failure path.

## ext_proc Flow

- **onRequestHeaders**: generate key → check request cacheability → coordinated
  lookup → `Hit` sends `ImmediateResponse`; `YouFill` continues to upstream;
  `TimedOut`/`Cancelled` continues without caching.
- **onResponseHeaders**: if filler, check response cacheability → if cacheable,
  start accumulating; set `mode_override` to `BUFFERED` for full body.
- **onResponseBody**: if storing, buffer body; on `end_of_stream`, store and
  report fill success.
- **onResponseTrailers**: if storing, save trailers, finalize and store.
- **onCancel**: report fill failure if filler.

## File Layout

| File | Contents |
|------|----------|
| `awaitable.h` | `Awaitable<T>` coroutine return type |
| `cache_types.h/.cc` | `CachedEntry`, `CoordinatedLookupResult`, `LookupStatus`, cache-control parsing, proto header helpers |
| `cache_store.h` | `CacheStore` abstract interface |
| `in_memory_cache_store.h/.cc` | `InMemoryCacheStore` default impl |
| `cache_key_generator.h` | `CacheKeyGenerator` interface + `DefaultCacheKeyGenerator` |
| `cacheability_checker.h` | `CacheabilityChecker` interface + `DefaultCacheabilityChecker` |
| `cache_age_calculator.h` | `CacheAgeCalculator` interface + `DefaultCacheAgeCalculator` |
| `cache_lookup_coordinator.h/.cc` | `CacheLookupCoordinator` (shared coalescing layer) |
| `cache_stream_handler.h/.cc` | `CacheStreamHandler` (per-stream ext_proc logic) |
| `server.h/.cc` | Updated reactor/service with handler integration |
| `main.cc` | Wires default implementations |
| `BUILD` | Build targets |
