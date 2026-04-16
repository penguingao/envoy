#include "source/extensions/filters/http/parallel_ext_proc/processor_chain.h"

#include "source/common/buffer/buffer_impl.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ParallelExtProc {

// Static default stream options.
const Http::ConnectionPool::Instance::StreamOptions ChainCallbacks::default_stream_options_{};

// --- ChainFilterManager ---

ChainFilterManager::ChainFilterManager(Http::FilterManagerCallbacks& callbacks,
                                       Event::Dispatcher& dispatcher,
                                       OptRef<const Network::Connection> connection,
                                       uint64_t stream_id,
                                       Buffer::BufferMemoryAccountSharedPtr account,
                                       uint64_t buffer_limit,
                                       StreamInfo::StreamInfo& parent_stream_info,
                                       ProcessorChain& chain)
    : FilterManager(callbacks, dispatcher, connection, stream_id, account,
                    /*proxy_100_continue=*/false, buffer_limit),
      parent_stream_info_(parent_stream_info), chain_(chain) {}

void ChainFilterManager::sendLocalReply(
    Http::Code, absl::string_view,
    const std::function<void(Http::ResponseHeaderMap&)>&,
    const absl::optional<Grpc::Status::GrpcStatus>, absl::string_view) {
  // Mark the filter chain as aborted, same as UpstreamFilterManager.
  state().decoder_filter_chain_aborted_ = true;
  state().encoder_filter_chain_aborted_ = true;
  state().encoder_filter_chain_complete_ = true;
  state().observed_encode_end_stream_ = true;
  // Notify the ProcessorChain of the error.
  chain_.onChainError();
}

// --- ChainCallbacks ---

ChainCallbacks::ChainCallbacks(ProcessorChain& chain, Http::RequestHeaderMap& request_headers,
                               Http::StreamDecoderFilterCallbacks& parent_callbacks)
    : chain_(chain), request_headers_(request_headers), parent_callbacks_(parent_callbacks) {}

void ChainCallbacks::onDecoderFilterAboveWriteBufferHighWatermark() {
  chain_.onSubChainAboveHighWatermark();
}

void ChainCallbacks::onDecoderFilterBelowWriteBufferLowWatermark() {
  chain_.onSubChainBelowLowWatermark();
}

Http::RequestHeaderMapOptRef ChainCallbacks::requestHeaders() {
  return {request_headers_};
}

Http::RequestTrailerMapOptRef ChainCallbacks::requestTrailers() {
  if (request_trailers_) {
    return {*request_trailers_};
  }
  return {};
}

Http::ResponseHeaderMapOptRef ChainCallbacks::informationalHeaders() {
  if (informational_headers_) {
    return {*informational_headers_};
  }
  return {};
}

Http::ResponseHeaderMapOptRef ChainCallbacks::responseHeaders() {
  if (response_headers_) {
    return {*response_headers_};
  }
  return {};
}

Http::ResponseTrailerMapOptRef ChainCallbacks::responseTrailers() {
  if (response_trailers_) {
    return {*response_trailers_};
  }
  return {};
}

OptRef<const Tracing::Config> ChainCallbacks::tracingConfig() const {
  return parent_callbacks_.tracingConfig();
}

const ScopeTrackedObject& ChainCallbacks::scope() {
  return parent_callbacks_.scope();
}

Tracing::Span& ChainCallbacks::activeSpan() {
  return parent_callbacks_.activeSpan();
}

OptRef<const Upstream::ClusterInfo> ChainCallbacks::clusterInfo() {
  return parent_callbacks_.clusterInfo();
}

Upstream::ClusterInfoConstSharedPtr ChainCallbacks::clusterInfoSharedPtr() {
  return parent_callbacks_.clusterInfoSharedPtr();
}

void ChainCallbacks::resetStream(Http::StreamResetReason, absl::string_view) {
  chain_.onChainError();
}

StreamInfo::StreamInfo& ChainCallbacks::upstreamStreamInfo() {
  return parent_callbacks_.streamInfo();
}

const Http::ConnectionPool::Instance::StreamOptions&
ChainCallbacks::upstreamStreamOptions() const {
  return default_stream_options_;
}

// --- ProcessorChain ---

ProcessorChain::ProcessorChain(uint32_t index, Http::FilterFactoryCb ext_proc_factory_cb,
                               uint32_t priority, bool wait_for_end_stream,
                               bool is_body_modifier, ChainCompleteCallback on_complete,
                               ChainErrorCallback on_error,
                               ChainWatermarkCallback on_high_watermark,
                               ChainWatermarkCallback on_low_watermark)
    : index_(index), ext_proc_factory_cb_(std::move(ext_proc_factory_cb)), priority_(priority),
      wait_for_end_stream_(wait_for_end_stream), is_body_modifier_(is_body_modifier),
      on_complete_(std::move(on_complete)), on_error_(std::move(on_error)),
      on_high_watermark_(std::move(on_high_watermark)),
      on_low_watermark_(std::move(on_low_watermark)) {}

const Buffer::Instance& ProcessorChain::modifiedBody() const {
  ASSERT(capture_filter_ != nullptr);
  return capture_filter_->modifiedBody();
}

ProcessorChain::~ProcessorChain() { cancel(); }

void ProcessorChain::startProcessing(Http::RequestHeaderMap& original_headers, bool end_stream,
                                     Http::StreamDecoderFilterCallbacks& parent_callbacks) {
  // Deep-copy request headers for this chain.
  headers_copy_ = Http::createHeaderMap<Http::RequestHeaderMapImpl>(original_headers);

  // Create the capture filter with our callback. The trigger mode determines
  // whether the parent is notified on decodeHeaders (streaming) or when
  // end_stream=true is observed (buffered). If this chain is the designated
  // body modifier, the CaptureFilter accumulates the post-ext_proc body so
  // the parent can use it as the body sent upstream.
  capture_filter_ = std::make_shared<CaptureFilter>(
      index_, wait_for_end_stream_, is_body_modifier_,
      [this](uint32_t idx, Http::RequestHeaderMap& hdrs) { onCaptureComplete(idx, hdrs); });

  // Create callbacks and filter manager.
  callbacks_ = std::make_unique<ChainCallbacks>(*this, *headers_copy_, parent_callbacks);
  filter_manager_ = std::make_unique<ChainFilterManager>(
      *callbacks_, parent_callbacks.dispatcher(), parent_callbacks.connection(),
      parent_callbacks.streamId(), parent_callbacks.account(), parent_callbacks.bufferLimit(),
      parent_callbacks.streamInfo(), *this);

  // Create the filter chain and feed headers.
  filter_manager_->createFilterChain(*this);
  filter_manager_->requestHeadersInitialized();
  filter_manager_->decodeHeaders(*headers_copy_, end_stream);
}

void ProcessorChain::forwardData(Buffer::Instance& data, bool end_stream) {
  // Note: completed_ is intentionally NOT checked here. In streaming mode, the
  // chain is marked "completed" (parent-notified) as soon as headers arrive at
  // the CaptureFilter, but the sub-chain must continue to receive body chunks
  // so its ext_proc can stream them to the processor.
  if (destroyed_ || failed_ || filter_manager_ == nullptr) {
    return;
  }
  // Copy the data so draining/buffering in the sub-chain's FilterManager does
  // not affect the caller's buffer.
  Buffer::OwnedImpl data_copy;
  data_copy.add(data);
  filter_manager_->decodeData(data_copy, end_stream);
}

void ProcessorChain::forwardTrailers(Http::RequestTrailerMap& trailers) {
  if (destroyed_ || failed_ || filter_manager_ == nullptr) {
    return;
  }
  filter_manager_->decodeTrailers(trailers);
}

void ProcessorChain::onSubChainAboveHighWatermark() {
  if (destroyed_ || failed_) {
    return;
  }
  if (on_high_watermark_) {
    on_high_watermark_();
  }
}

void ProcessorChain::onSubChainBelowLowWatermark() {
  if (destroyed_ || failed_) {
    return;
  }
  if (on_low_watermark_) {
    on_low_watermark_();
  }
}

void ProcessorChain::cancel() {
  if (destroyed_) {
    return;
  }
  destroyed_ = true;
  if (filter_manager_) {
    filter_manager_->destroyFilters();
  }
}

void ProcessorChain::onCaptureComplete(uint32_t, Http::RequestHeaderMap& modified_headers) {
  if (completed_ || failed_) {
    return;
  }
  completed_ = true;
  on_complete_(index_, modified_headers);
}

void ProcessorChain::onChainError() {
  if (completed_ || failed_) {
    return;
  }
  failed_ = true;
  on_error_(index_);
}

bool ProcessorChain::createFilterChain(
    Http::FilterChainFactoryCallbacks& callbacks) const {
  // Add the ext_proc filter.
  ext_proc_factory_cb_(callbacks);
  // Add the capture terminal filter.
  callbacks.addStreamDecoderFilter(capture_filter_);
  return true;
}

} // namespace ParallelExtProc
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
