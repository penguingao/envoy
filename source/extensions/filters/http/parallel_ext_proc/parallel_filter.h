#pragma once

#include <memory>
#include <vector>

#include "envoy/event/timer.h"
#include "envoy/http/filter.h"

#include "source/common/common/logger.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"
#include "source/extensions/filters/http/parallel_ext_proc/header_merger.h"
#include "source/extensions/filters/http/parallel_ext_proc/processor_chain.h"

#include "envoy/extensions/filters/http/ext_proc/v3/ext_proc.pb.h"
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
  envoy::extensions::filters::http::ext_proc::v3::ProcessingMode processing_mode;
};

// Execution mode selected at config time based on the aggregate processing
// mode across all processors.
enum class ExecutionMode {
  // At least one processor requires accumulating the body (BUFFERED, STREAMED,
  // BUFFERED_PARTIAL). The main chain buffers the body with watermark flow
  // control and waits for every sub-chain to finish before the body is
  // released to the upstream. Processor failures can cleanly reject the
  // request with a local reply.
  Buffered,
  // Every processor has request_body_mode in {NONE, FULL_DUPLEX_STREAMED}.
  // The main chain does not buffer the body; chunks are forwarded to
  // sub-chains and then released to the upstream. Flow control is managed
  // by aggregating watermark signals from every sub-chain. Processor
  // failures during body streaming cannot cleanly abort (body bytes may
  // already be sent upstream).
  Streaming,
};

class ParallelFilterConfig {
public:
  ParallelFilterConfig(
      std::vector<ProcessorInfo>&& processors,
      const envoy::extensions::filters::http::parallel_ext_proc::v3::FailurePolicy&
          failure_policy,
      std::chrono::milliseconds aggregate_timeout);

  const std::vector<ProcessorInfo>& processors() const { return processors_; }
  const envoy::extensions::filters::http::parallel_ext_proc::v3::FailurePolicy&
  failurePolicy() const {
    return failure_policy_;
  }
  std::chrono::milliseconds aggregateTimeout() const { return aggregate_timeout_; }
  ExecutionMode mode() const { return mode_; }

  // Index of the single processor designated as the body modifier, or
  // absl::nullopt if no processor has can_modify_body=true.
  absl::optional<uint32_t> bodyModifierIndex() const { return body_modifier_index_; }

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
  ExecutionMode mode_;
  absl::optional<uint32_t> body_modifier_index_;
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
  Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;
  Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap& trailers) override;

  // Called by a ProcessorChain when its ext_proc signals that its sidestream
  // buffer is above the high watermark (processor is slow). The parallel
  // filter aggregates these signals and, when any sub-chain is above high
  // watermark, propagates to the main chain to backpressure the downstream.
  void onChainAboveHighWatermark();
  void onChainBelowLowWatermark();

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
  // Number of sub-chains currently above their high watermark. When > 0 we
  // signal our decoder_callbacks_ to backpressure the main downstream.
  uint32_t chains_above_high_watermark_{0};
  bool finalized_{false};
  bool downstream_backpressured_{false};
  Event::TimerPtr aggregate_timer_;
};

} // namespace ParallelExtProc
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
