#pragma once

#include <memory>
#include <vector>

#include "envoy/event/timer.h"
#include "envoy/http/filter.h"

#include "source/common/common/logger.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"
#include "source/extensions/filters/http/parallel_ext_proc/header_merger.h"
#include "source/extensions/filters/http/parallel_ext_proc/processor_chain.h"

#include "envoy/extensions/filters/http/parallel_ext_proc/v3/parallel_ext_proc.pb.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ParallelExtProc {

// Shared configuration for the parallel ext_proc filter.
struct ProcessorInfo {
  std::string name;
  uint32_t priority;
  Http::FilterFactoryCb ext_proc_factory_cb;
  bool can_modify_body;
};

class ParallelFilterConfig {
public:
  ParallelFilterConfig(
      std::vector<ProcessorInfo>&& processors,
      const envoy::extensions::filters::http::parallel_ext_proc::v3::FailurePolicy&
          failure_policy,
      std::chrono::milliseconds aggregate_timeout)
      : processors_(std::move(processors)), failure_policy_(failure_policy),
        aggregate_timeout_(aggregate_timeout) {}

  const std::vector<ProcessorInfo>& processors() const { return processors_; }
  const envoy::extensions::filters::http::parallel_ext_proc::v3::FailurePolicy&
  failurePolicy() const {
    return failure_policy_;
  }
  std::chrono::milliseconds aggregateTimeout() const { return aggregate_timeout_; }

  uint32_t minSuccessCount() const {
    // 0 means all must succeed.
    return failure_policy_.min_success_count() == 0
               ? static_cast<uint32_t>(processors_.size())
               : failure_policy_.min_success_count();
  }

private:
  const std::vector<ProcessorInfo> processors_;
  const envoy::extensions::filters::http::parallel_ext_proc::v3::FailurePolicy failure_policy_;
  const std::chrono::milliseconds aggregate_timeout_;
};

using ParallelFilterConfigSharedPtr = std::shared_ptr<ParallelFilterConfig>;

// Main filter that sits in the downstream HTTP filter chain.
// Fans out request headers to N ext_proc processors in parallel,
// merges their mutations by priority, and continues the chain.
class ParallelExtProcFilter : public Http::PassThroughFilter,
                              public Logger::Loggable<Logger::Id::ext_proc> {
public:
  explicit ParallelExtProcFilter(ParallelFilterConfigSharedPtr config);

  // Http::StreamFilterBase
  void onDestroy() override;

  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;

  // Phase 2: response path
  // Http::StreamEncoderFilter
  // Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap& headers,
  //                                         bool end_stream) override;

private:
  // Called by ProcessorChain when ext_proc completes for a processor.
  void onProcessorComplete(uint32_t index, Http::RequestHeaderMap& modified_headers);

  // Called by ProcessorChain on error.
  void onProcessorError(uint32_t index);

  // Called when the aggregate timeout fires.
  void onAggregateTimeout();

  // Check if all chains are done and finalize if so.
  void maybeFinalize();

  ParallelFilterConfigSharedPtr config_;
  std::vector<std::unique_ptr<ProcessorChain>> chains_;
  // Original headers snapshot for diffing.
  Http::RequestHeaderMapPtr original_headers_snapshot_;
  // Collected deltas from completed processors (index -> delta).
  std::vector<std::pair<uint32_t, HeaderDelta>> completed_deltas_;
  uint32_t completed_count_{0};
  uint32_t failed_count_{0};
  bool finalized_{false};
  Event::TimerPtr aggregate_timer_;
};

} // namespace ParallelExtProc
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
