#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "envoy/service/ext_proc/v3/external_processor.pb.h"

#include "contrib/ext_proc_cache/awaitable.h"
#include "contrib/ext_proc_cache/cache_age_calculator.h"
#include "contrib/ext_proc_cache/cache_key_generator.h"
#include "contrib/ext_proc_cache/cache_lookup_coordinator.h"
#include "contrib/ext_proc_cache/cache_types.h"
#include "contrib/ext_proc_cache/cacheability_checker.h"

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

using ProcessingResponse = envoy::service::ext_proc::v3::ProcessingResponse;

// Result of handling a request. Contains the initial response to write, and
// optionally a body reader for streaming cache hits. When body_reader is set,
// the reactor streams body chunks one at a time instead of buffering them all.
struct HandleResult {
  ProcessingResponse response;
  std::unique_ptr<CacheBodyReader> body_reader;
  size_t chunk_size = 0;
};

// Per-stream handler that implements the ext_proc caching protocol state machine.
class CacheStreamHandler {
public:
  // chunk_size controls how large each body chunk is when serving cached entries
  // via StreamedImmediateResponse. 0 means send the entire body in one chunk.
  CacheStreamHandler(std::shared_ptr<CacheLookupCoordinator> coordinator,
                     std::shared_ptr<CacheKeyGenerator> key_gen,
                     std::shared_ptr<CacheabilityChecker> cacheability,
                     std::shared_ptr<CacheAgeCalculator> age_calc,
                     std::chrono::system_clock::time_point deadline,
                     size_t chunk_size = 65536);

  // Returns the initial response and optionally a body reader for streaming.
  Awaitable<HandleResult>
  onRequestHeaders(const envoy::service::ext_proc::v3::HttpHeaders& headers);

  // Handles response headers. May return a body reader for streaming.
  Awaitable<HandleResult>
  onResponseHeaders(const envoy::service::ext_proc::v3::HttpHeaders& headers);

  // May co_await store on end_of_stream.
  Awaitable<ProcessingResponse>
  onResponseBody(const envoy::service::ext_proc::v3::HttpBody& body);

  // May co_await store for trailers case.
  Awaitable<ProcessingResponse>
  onResponseTrailers(const envoy::service::ext_proc::v3::HttpTrailers& trailers);

  // Called when the stream is cancelled.
  void onCancel();

private:
  // Build the headers-only StreamedImmediateResponse for a cache hit.
  // Returns HandleResult with the headers response + body reader for the
  // reactor to stream from. Only valid in response to request_headers.
  HandleResult buildCacheHitResult(const CacheEntryMetadata& metadata,
                                   std::unique_ptr<CacheBodyReader> reader,
                                   std::optional<Seconds> age = std::nullopt);

  // Build a single ImmediateResponse for a cached entry.
  // Used in response to response_headers (where StreamedImmediateResponse
  // is not supported). Requires the full body as a string.
  static ProcessingResponse buildImmediateCacheResponse(const CacheEntryMetadata& metadata,
                                                        const std::string& body);

  std::shared_ptr<CacheLookupCoordinator> coordinator_;
  std::shared_ptr<CacheKeyGenerator> key_gen_;
  std::shared_ptr<CacheabilityChecker> cacheability_;
  std::shared_ptr<CacheAgeCalculator> age_calc_;
  std::chrono::system_clock::time_point deadline_;
  size_t chunk_size_;

  // Per-stream state.
  std::string current_key_;
  bool is_filler_ = false;
  bool storing_ = false;
  bool validating_ = false;
  CachedEntry pending_entry_; // used only for 304 re-store path
  std::optional<CacheEntryMetadata> stale_metadata_;
  std::unique_ptr<CacheBodyReader> stale_reader_;
  ProtoHeaderMap saved_request_headers_;
};

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
