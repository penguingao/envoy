#pragma once

#include <chrono>
#include <memory>
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

// Per-stream handler that implements the ext_proc caching protocol state machine.
class CacheStreamHandler {
public:
  CacheStreamHandler(std::shared_ptr<CacheLookupCoordinator> coordinator,
                     std::shared_ptr<CacheKeyGenerator> key_gen,
                     std::shared_ptr<CacheabilityChecker> cacheability,
                     std::shared_ptr<CacheAgeCalculator> age_calc,
                     std::chrono::system_clock::time_point deadline);

  // Returns response for each ext_proc phase.
  Awaitable<ProcessingResponse>
  onRequestHeaders(const envoy::service::ext_proc::v3::HttpHeaders& headers);

  // Sync: decides whether to cache, sets mode_override for body.
  ProcessingResponse onResponseHeaders(const envoy::service::ext_proc::v3::HttpHeaders& headers);

  // May co_await store on end_of_stream.
  Awaitable<ProcessingResponse>
  onResponseBody(const envoy::service::ext_proc::v3::HttpBody& body);

  // May co_await store for trailers case.
  Awaitable<ProcessingResponse>
  onResponseTrailers(const envoy::service::ext_proc::v3::HttpTrailers& trailers);

  // Called when the stream is cancelled.
  void onCancel();

private:
  std::shared_ptr<CacheLookupCoordinator> coordinator_;
  std::shared_ptr<CacheKeyGenerator> key_gen_;
  std::shared_ptr<CacheabilityChecker> cacheability_;
  std::shared_ptr<CacheAgeCalculator> age_calc_;
  std::chrono::system_clock::time_point deadline_;

  // Per-stream state.
  std::string current_key_;
  bool is_filler_ = false;
  bool storing_ = false;
  CachedEntry pending_entry_;
  ProtoHeaderMap saved_request_headers_;
};

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
