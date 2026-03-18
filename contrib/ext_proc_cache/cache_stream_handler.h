#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

  // Returns one or more responses (multiple for StreamedImmediateResponse).
  Awaitable<std::vector<ProcessingResponse>>
  onRequestHeaders(const envoy::service::ext_proc::v3::HttpHeaders& headers);

  // Handles response headers. May return multiple responses for cached entries.
  Awaitable<std::vector<ProcessingResponse>>
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
  // Build a StreamedImmediateResponse sequence by streaming body from reader.
  // First message has headers (with :status), followed by body chunks.
  // Only valid in response to request_headers.
  Awaitable<std::vector<ProcessingResponse>>
  buildStreamedCacheResponse(const CacheEntryMetadata& metadata, CacheBodyReader& reader,
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
  CachedEntry pending_entry_;
  std::optional<CacheEntryMetadata> stale_metadata_;
  std::unique_ptr<CacheBodyReader> stale_reader_;
  ProtoHeaderMap saved_request_headers_;
};

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
