#include "source/extensions/filters/http/parallel_ext_proc/parallel_filter.h"

#include "source/common/common/assert.h"
#include "source/common/http/header_map_impl.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ParallelExtProc {

namespace {
using ExtProcProcessingMode = envoy::extensions::filters::http::ext_proc::v3::ProcessingMode;
} // namespace

ParallelFilterConfig::ParallelFilterConfig(
    std::vector<ProcessorInfo>&& processors,
    const envoy::extensions::filters::http::parallel_ext_proc::v3::FailurePolicy& failure_policy,
    std::chrono::milliseconds aggregate_timeout)
    : processors_(std::move(processors)), failure_policy_(failure_policy),
      aggregate_timeout_(aggregate_timeout), mode_(ExecutionMode::Buffered) {
  // Find the designated body modifier (at most one).
  for (uint32_t i = 0; i < processors_.size(); ++i) {
    if (processors_[i].can_modify_body) {
      if (body_modifier_index_.has_value()) {
        throw EnvoyException(fmt::format(
            "parallel ext_proc: at most one processor may set can_modify_body=true; "
            "found it set on both '{}' and '{}'",
            processors_[body_modifier_index_.value()].name, processors_[i].name));
      }
      // The modifier must actually see the body, and must see it in a mode
      // where ext_proc applies mutations to the filter chain (not streaming).
      const auto body_mode = processors_[i].processing_mode.request_body_mode();
      if (body_mode != ExtProcProcessingMode::BUFFERED &&
          body_mode != ExtProcProcessingMode::STREAMED &&
          body_mode != ExtProcProcessingMode::BUFFERED_PARTIAL) {
        throw EnvoyException(fmt::format(
            "parallel ext_proc: processor '{}' has can_modify_body=true but its "
            "request_body_mode is not BUFFERED, STREAMED, or BUFFERED_PARTIAL",
            processors_[i].name));
      }
      body_modifier_index_ = i;
    }
  }

  // Classify execution mode based on aggregate body modes across processors.
  // Streaming mode is selected only if every processor has request_body_mode
  // in {NONE, FULL_DUPLEX_STREAMED}. Any other body mode forces buffered
  // mode so we can retain the ability to cleanly reject requests on failure.
  bool all_streaming_compatible = true;
  for (const auto& p : processors_) {
    const auto body_mode = p.processing_mode.request_body_mode();
    if (body_mode != ExtProcProcessingMode::NONE &&
        body_mode != ExtProcProcessingMode::FULL_DUPLEX_STREAMED) {
      all_streaming_compatible = false;
      break;
    }
  }
  if (all_streaming_compatible) {
    mode_ = ExecutionMode::Streaming;
    // FULL_DUPLEX_STREAMED processors require request_trailer_mode=SEND; this
    // is inherited validation from the ext_proc filter applied per processor.
    for (const auto& p : processors_) {
      if (p.processing_mode.request_body_mode() == ExtProcProcessingMode::FULL_DUPLEX_STREAMED &&
          p.processing_mode.request_trailer_mode() != ExtProcProcessingMode::SEND) {
        throw EnvoyException(fmt::format(
            "parallel ext_proc: processor '{}' has request_body_mode=FULL_DUPLEX_STREAMED "
            "which requires request_trailer_mode=SEND",
            p.name));
      }
    }
  }

  // A body modifier is only supported in buffered mode. If the config
  // produced streaming mode yet has a modifier, the modifier's body mode
  // rules above should have already forced buffered mode. Sanity-check and
  // reject explicitly for clarity.
  if (body_modifier_index_.has_value() && mode_ == ExecutionMode::Streaming) {
    throw EnvoyException(
        "parallel ext_proc: can_modify_body=true is only supported in buffered mode");
  }
}

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

  // In buffered mode the completion signal is end_stream on the CaptureFilter
  // (wait for the entire request). In streaming mode it is the first
  // decodeHeaders on the CaptureFilter (parent releases main chain early).
  const bool wait_for_end_stream = config_->mode() == ExecutionMode::Buffered;

  for (uint32_t i = 0; i < processors.size(); ++i) {
    const bool is_body_modifier =
        config_->bodyModifierIndex().has_value() && config_->bodyModifierIndex().value() == i;
    auto chain = std::make_unique<ProcessorChain>(
        i, processors[i].ext_proc_factory_cb, processors[i].priority, wait_for_end_stream,
        is_body_modifier,
        [this](uint32_t idx, Http::RequestHeaderMap& modified) {
          onProcessorComplete(idx, modified);
        },
        [this](uint32_t idx) { onProcessorError(idx); },
        [this]() { onChainAboveHighWatermark(); },
        [this]() { onChainBelowLowWatermark(); });

    chain->startProcessing(headers, end_stream, *decoder_callbacks_);
    chains_.push_back(std::move(chain));
  }

  // Start aggregate timeout if configured.
  if (config_->aggregateTimeout().count() > 0) {
    aggregate_timer_ =
        decoder_callbacks_->dispatcher().createTimer([this]() { onAggregateTimeout(); });
    aggregate_timer_->enableTimer(config_->aggregateTimeout());
  }

  // StopIteration pauses headers iteration on subsequent filters but still
  // allows body data to arrive at our decodeData, where we forward it to
  // every sub-chain so each ext_proc processor sees the body.
  return Http::FilterHeadersStatus::StopIteration;
}

Http::FilterDataStatus ParallelExtProcFilter::decodeData(Buffer::Instance& data, bool end_stream) {
  // Forward to every sub-chain so its ext_proc can observe/modify the body.
  for (auto& chain : chains_) {
    if (chain && !chain->failed()) {
      chain->forwardData(data, end_stream);
    }
  }

  if (config_->mode() == ExecutionMode::Buffered) {
    // Buffered mode: hold the data in the main FM until every sub-chain has
    // reported end_stream. The slowest processor dictates body processing
    // speed via the high watermark signal.
    return Http::FilterDataStatus::StopIterationAndWatermark;
  }

  // Streaming mode. If headers have already been merged and the main chain
  // has been released, let the body flow through to the upstream. Otherwise
  // hold the body locally until headers are merged. In both cases, flow
  // control is provided by aggregating watermark signals from each
  // sub-chain's ext_proc (onChainAboveHighWatermark).
  if (finalized_) {
    return Http::FilterDataStatus::Continue;
  }
  return Http::FilterDataStatus::StopIterationAndBuffer;
}

Http::FilterTrailersStatus
ParallelExtProcFilter::decodeTrailers(Http::RequestTrailerMap& trailers) {
  // Forward trailers to every active sub-chain.
  for (auto& chain : chains_) {
    if (chain && !chain->failed()) {
      chain->forwardTrailers(trailers);
    }
  }
  if (config_->mode() == ExecutionMode::Streaming && finalized_) {
    return Http::FilterTrailersStatus::Continue;
  }
  return Http::FilterTrailersStatus::StopIteration;
}

void ParallelExtProcFilter::onChainAboveHighWatermark() {
  chains_above_high_watermark_++;
  if (!downstream_backpressured_ && chains_above_high_watermark_ > 0 &&
      decoder_callbacks_ != nullptr) {
    downstream_backpressured_ = true;
    decoder_callbacks_->onDecoderFilterAboveWriteBufferHighWatermark();
  }
}

void ParallelExtProcFilter::onChainBelowLowWatermark() {
  ASSERT(chains_above_high_watermark_ > 0);
  chains_above_high_watermark_--;
  if (downstream_backpressured_ && chains_above_high_watermark_ == 0 &&
      decoder_callbacks_ != nullptr) {
    downstream_backpressured_ = false;
    decoder_callbacks_->onDecoderFilterBelowWriteBufferLowWatermark();
  }
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

  // If a body modifier is designated, replace the main chain's buffered body
  // with the body accumulated by the modifier's CaptureFilter (i.e. what the
  // modifier's ext_proc emitted after applying the processor's mutations).
  // Observer processors' body mutations are ignored.
  if (config_->bodyModifierIndex().has_value()) {
    const uint32_t modifier_idx = config_->bodyModifierIndex().value();
    if (modifier_idx < chains_.size() && chains_[modifier_idx] != nullptr &&
        chains_[modifier_idx]->completed()) {
      const Buffer::Instance& modifier_body = chains_[modifier_idx]->modifiedBody();
      decoder_callbacks_->modifyDecodingBuffer(
          [&modifier_body](Buffer::Instance& buffer) {
            buffer.drain(buffer.length());
            buffer.add(modifier_body);
          });
    }
  }

  // We are inside a sub-chain FilterManager's decodeHeaders callback (via CaptureFilter).
  // We cannot destroy the sub-chain FilterManagers here (filter_call_state_ != 0).
  // Defer cleanup and continueDecoding to the next event loop iteration.
  //
  // In streaming mode, sub-chains must remain alive so they can continue
  // receiving forwarded body/trailers and streaming them to their processors.
  // Cleanup happens at onDestroy().
  const bool cancel_on_continue = config_->mode() == ExecutionMode::Buffered;
  decoder_callbacks_->dispatcher().post([this, cancel_on_continue]() {
    if (cancel_on_continue) {
      for (auto& chain : chains_) {
        if (chain) {
          chain->cancel();
        }
      }
      chains_.clear();
    }
    decoder_callbacks_->continueDecoding();
  });
}

} // namespace ParallelExtProc
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
