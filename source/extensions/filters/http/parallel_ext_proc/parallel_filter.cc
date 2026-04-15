#include "source/extensions/filters/http/parallel_ext_proc/parallel_filter.h"

#include "source/common/http/header_map_impl.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ParallelExtProc {

ParallelExtProcFilter::ParallelExtProcFilter(ParallelFilterConfigSharedPtr config)
    : config_(std::move(config)) {}

void ParallelExtProcFilter::onDestroy() {
  if (aggregate_timer_) {
    aggregate_timer_->disableTimer();
  }
  for (auto& chain : chains_) {
    if (chain) {
      chain->cancel();
    }
  }
}

Http::FilterHeadersStatus
ParallelExtProcFilter::decodeHeaders(Http::RequestHeaderMap& headers, bool end_stream) {
  // Snapshot original headers for diffing later.
  original_headers_snapshot_ = Http::createHeaderMap<Http::RequestHeaderMapImpl>(headers);

  const auto& processors = config_->processors();
  chains_.reserve(processors.size());

  for (uint32_t i = 0; i < processors.size(); ++i) {
    auto chain = std::make_unique<ProcessorChain>(
        i, processors[i].ext_proc_factory_cb, processors[i].priority,
        [this](uint32_t idx, Http::RequestHeaderMap& modified) {
          onProcessorComplete(idx, modified);
        },
        [this](uint32_t idx) { onProcessorError(idx); });

    chain->startProcessing(headers, end_stream, *decoder_callbacks_);
    chains_.push_back(std::move(chain));
  }

  // Start aggregate timeout if configured.
  if (config_->aggregateTimeout().count() > 0) {
    aggregate_timer_ =
        decoder_callbacks_->dispatcher().createTimer([this]() { onAggregateTimeout(); });
    aggregate_timer_->enableTimer(config_->aggregateTimeout());
  }

  return Http::FilterHeadersStatus::StopIteration;
}

void ParallelExtProcFilter::onProcessorComplete(uint32_t index,
                                                Http::RequestHeaderMap& modified_headers) {
  if (finalized_) {
    return;
  }

  // Compute diff between original and modified headers.
  HeaderDelta delta = HeaderMerger::diffHeaders(*original_headers_snapshot_, modified_headers);
  completed_deltas_.emplace_back(chains_[index]->priority(), std::move(delta));
  completed_count_++;

  maybeFinalize();
}

void ParallelExtProcFilter::onProcessorError(uint32_t index) {
  if (finalized_) {
    return;
  }

  ENVOY_LOG(warn, "parallel ext_proc: processor {} (index {}) failed",
            config_->processors()[index].name, index);
  failed_count_++;

  maybeFinalize();
}

void ParallelExtProcFilter::onAggregateTimeout() {
  if (finalized_) {
    return;
  }

  ENVOY_LOG(warn, "parallel ext_proc: aggregate timeout reached with {}/{} processors complete",
            completed_count_, chains_.size());

  // Cancel any remaining chains.
  for (auto& chain : chains_) {
    if (chain && !chain->completed() && !chain->failed()) {
      chain->cancel();
      failed_count_++;
    }
  }

  maybeFinalize();
}

void ParallelExtProcFilter::maybeFinalize() {
  if (finalized_) {
    return;
  }

  const uint32_t total = static_cast<uint32_t>(chains_.size());
  if (completed_count_ + failed_count_ < total) {
    // Check early termination: can we still meet min_success_count?
    const uint32_t remaining = total - completed_count_ - failed_count_;
    if (completed_count_ + remaining < config_->minSuccessCount() &&
        !config_->failurePolicy().failure_mode_allow()) {
      finalized_ = true;
      if (aggregate_timer_) {
        aggregate_timer_->disableTimer();
      }
      decoder_callbacks_->sendLocalReply(Http::Code::InternalServerError,
                                         "parallel ext_proc: insufficient processor responses",
                                         nullptr, absl::nullopt, "parallel_ext_proc_failure");
      return;
    }
    return; // Still waiting for more responses.
  }

  finalized_ = true;
  if (aggregate_timer_) {
    aggregate_timer_->disableTimer();
  }

  // Check if we have enough successes.
  if (completed_count_ < config_->minSuccessCount()) {
    if (config_->failurePolicy().failure_mode_allow()) {
      ENVOY_LOG(warn, "parallel ext_proc: fail-open with {}/{} successes", completed_count_,
                total);
    } else {
      decoder_callbacks_->sendLocalReply(Http::Code::InternalServerError,
                                         "parallel ext_proc: insufficient processor responses",
                                         nullptr, absl::nullopt, "parallel_ext_proc_failure");
      return;
    }
  }

  // Merge and apply header mutations.
  if (!completed_deltas_.empty()) {
    HeaderMerger::mergeAndApply(completed_deltas_,
                                *decoder_callbacks_->requestHeaders());
  }

  // We are inside a sub-chain FilterManager's decodeHeaders callback (via CaptureFilter).
  // We cannot destroy the sub-chain FilterManagers here (filter_call_state_ != 0).
  // Defer cleanup and continueDecoding to the next event loop iteration.
  decoder_callbacks_->dispatcher().post([this]() {
    for (auto& chain : chains_) {
      if (chain) {
        chain->cancel();
      }
    }
    chains_.clear();
    decoder_callbacks_->continueDecoding();
  });
}

} // namespace ParallelExtProc
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
