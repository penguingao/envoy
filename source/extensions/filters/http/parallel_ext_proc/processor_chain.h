#pragma once

#include <functional>
#include <memory>

#include "envoy/http/filter.h"
#include "envoy/network/connection.h"

#include "source/common/common/logger.h"
#include "source/common/http/filter_manager.h"
#include "source/common/http/header_map_impl.h"
#include "source/extensions/filters/http/parallel_ext_proc/capture_filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ParallelExtProc {

class ProcessorChain;

// Callback types for chain completion/error/watermark.
using ChainCompleteCallback =
    std::function<void(uint32_t index, Http::RequestHeaderMap& modified_headers)>;
using ChainErrorCallback = std::function<void(uint32_t index)>;
using ChainWatermarkCallback = std::function<void()>;

// FilterManager subclass for processor chains. Follows the UpstreamFilterManager pattern
// from source/common/router/upstream_request.cc:46-78.
class ChainFilterManager : public Http::FilterManager {
public:
  ChainFilterManager(Http::FilterManagerCallbacks& callbacks, Event::Dispatcher& dispatcher,
                     OptRef<const Network::Connection> connection, uint64_t stream_id,
                     Buffer::BufferMemoryAccountSharedPtr account, uint64_t buffer_limit,
                     StreamInfo::StreamInfo& parent_stream_info, ProcessorChain& chain);

  StreamInfo::StreamInfo& streamInfo() override { return parent_stream_info_; }
  const StreamInfo::StreamInfo& streamInfo() const override { return parent_stream_info_; }

  void sendLocalReply(Http::Code code, absl::string_view body,
                      const std::function<void(Http::ResponseHeaderMap& headers)>& modify_headers,
                      const absl::optional<Grpc::Status::GrpcStatus> grpc_status,
                      absl::string_view details) override;

  void executeLocalReplyIfPrepared() override {}

private:
  StreamInfo::StreamInfo& parent_stream_info_;
  ProcessorChain& chain_;
};

// FilterManagerCallbacks implementation for processor chains.
// Follows UpstreamRequestFilterManagerCallbacks from
// source/common/router/upstream_request.h:267-394.
class ChainCallbacks : public Http::FilterManagerCallbacks,
                       public Http::UpstreamStreamFilterCallbacks {
public:
  ChainCallbacks(ProcessorChain& chain, Http::RequestHeaderMap& request_headers,
                 Http::StreamDecoderFilterCallbacks& parent_callbacks);

  // FilterManagerCallbacks - encode direction (response path, no-op for phase 1)
  void encodeHeaders(Http::ResponseHeaderMap&, bool) override {}
  void encode1xxHeaders(Http::ResponseHeaderMap&) override {}
  void encodeData(Buffer::Instance&, bool) override {}
  void encodeTrailers(Http::ResponseTrailerMap&) override {}
  void encodeMetadata(Http::MetadataMapPtr&&) override {}

  // FilterManagerCallbacks - state accessors
  void setRequestTrailers(Http::RequestTrailerMapPtr&& request_trailers) override {
    request_trailers_ = std::move(request_trailers);
  }
  void setInformationalHeaders(Http::ResponseHeaderMapPtr&& headers) override {
    informational_headers_ = std::move(headers);
  }
  void setResponseHeaders(Http::ResponseHeaderMapPtr&& headers) override {
    response_headers_ = std::move(headers);
  }
  void setResponseTrailers(Http::ResponseTrailerMapPtr&& trailers) override {
    response_trailers_ = std::move(trailers);
  }
  Http::RequestHeaderMapOptRef requestHeaders() override;
  Http::RequestTrailerMapOptRef requestTrailers() override;
  Http::ResponseHeaderMapOptRef informationalHeaders() override;
  Http::ResponseHeaderMapOptRef responseHeaders() override;
  Http::ResponseTrailerMapOptRef responseTrailers() override;

  // FilterManagerCallbacks - delegated to parent
  OptRef<const Tracing::Config> tracingConfig() const override;
  const ScopeTrackedObject& scope() override;
  Tracing::Span& activeSpan() override;
  OptRef<const Upstream::ClusterInfo> clusterInfo() override;
  Upstream::ClusterInfoConstSharedPtr clusterInfoSharedPtr() override;

  // FilterManagerCallbacks - stream control. The watermark signals bubble up
  // from the sub-chain's ext_proc when its gRPC sidestream buffer fills up
  // (processor is slow). We forward them to the ProcessorChain so the parent
  // parallel filter can aggregate watermarks from every chain and apply
  // backpressure to the downstream.
  void onDecoderFilterBelowWriteBufferLowWatermark() override;
  void onDecoderFilterAboveWriteBufferHighWatermark() override;
  void disarmRequestTimeout() override {}
  void resetIdleTimer() override {}
  void endStream() override {}
  void onLocalReply(Http::Code) override {}
  void sendGoAwayAndClose(bool) override {}
  void onResponseDataTooLarge() override {}
  void onRequestDataTooLarge() override {}
  const Router::RouteEntry::UpgradeMap* upgradeMap() override { return nullptr; }
  Http::Http1StreamEncoderOptionsOptRef http1StreamEncoderOptions() override { return {}; }
  void recreateStream(StreamInfo::FilterStateSharedPtr) override {}
  void resetStream(Http::StreamResetReason reset_reason = Http::StreamResetReason::LocalReset,
                   absl::string_view transport_failure_reason = "") override;

  // UpstreamStreamFilterCallbacks
  OptRef<Http::UpstreamStreamFilterCallbacks> upstreamCallbacks() override { return {*this}; }
  StreamInfo::StreamInfo& upstreamStreamInfo() override;
  OptRef<Router::GenericUpstream> upstream() override { return {}; }
  bool pausedForConnect() const override { return false; }
  void setPausedForConnect(bool) override {}
  bool pausedForWebsocketUpgrade() const override { return false; }
  void setPausedForWebsocketUpgrade(bool) override {}
  const Http::ConnectionPool::Instance::StreamOptions& upstreamStreamOptions() const override;
  void addUpstreamCallbacks(Http::UpstreamCallbacks&) override {}
  void
  setUpstreamToDownstream(Router::UpstreamToDownstream&) override {}
  bool isHalfCloseEnabled() override { return false; }
  void disableRouteTimeoutForWebsocketUpgrade() override {}
  void disablePerTryTimeoutForWebsocketUpgrade() override {}
  void dumpState(std::ostream&, int) const override {}

  // FilterManagerCallbacks - downstream not available
  OptRef<Http::DownstreamStreamFilterCallbacks> downstreamCallbacks() override { return {}; }

private:
  ProcessorChain& chain_;
  Http::RequestHeaderMap& request_headers_;
  Http::StreamDecoderFilterCallbacks& parent_callbacks_;
  Http::RequestTrailerMapPtr request_trailers_;
  Http::ResponseHeaderMapPtr informational_headers_;
  Http::ResponseHeaderMapPtr response_headers_;
  Http::ResponseTrailerMapPtr response_trailers_;
  static const Http::ConnectionPool::Instance::StreamOptions default_stream_options_;
};

// Manages a single FilterManager + ext_proc filter for one processor.
// Implements FilterChainFactory to provide the filter chain.
class ProcessorChain : public Http::FilterChainFactory,
                       public Logger::Loggable<Logger::Id::ext_proc> {
public:
  ProcessorChain(uint32_t index, Http::FilterFactoryCb ext_proc_factory_cb,
                 uint32_t priority, bool wait_for_end_stream,
                 ChainCompleteCallback on_complete, ChainErrorCallback on_error,
                 ChainWatermarkCallback on_high_watermark,
                 ChainWatermarkCallback on_low_watermark);
  ~ProcessorChain();

  // Start processing by copying headers and feeding them through the filter chain.
  void startProcessing(Http::RequestHeaderMap& original_headers, bool end_stream,
                       Http::StreamDecoderFilterCallbacks& parent_callbacks);

  // Forward a copy of request body data to this chain's FilterManager.
  void forwardData(Buffer::Instance& data, bool end_stream);

  // Forward request trailers to this chain's FilterManager.
  void forwardTrailers(Http::RequestTrailerMap& trailers);

  // Cancel processing and clean up.
  void cancel();

  // Called by CaptureFilter when it receives the modified headers.
  void onCaptureComplete(uint32_t index, Http::RequestHeaderMap& modified_headers);

  // Called by ChainFilterManager when sendLocalReply is invoked.
  void onChainError();

  // Called by ChainCallbacks when the sub-chain's ext_proc signals a
  // sidestream watermark change. Forwarded to the parent via callbacks.
  void onSubChainAboveHighWatermark();
  void onSubChainBelowLowWatermark();

  uint32_t index() const { return index_; }
  uint32_t priority() const { return priority_; }
  bool completed() const { return completed_; }
  bool failed() const { return failed_; }

  // Http::FilterChainFactory
  bool createFilterChain(Http::FilterChainFactoryCallbacks& callbacks) const override;
  bool createUpgradeFilterChain(absl::string_view, const Http::FilterChainFactory::UpgradeMap*,
                                Http::FilterChainFactoryCallbacks&) const override {
    return false;
  }

private:
  const uint32_t index_;
  Http::FilterFactoryCb ext_proc_factory_cb_;
  const uint32_t priority_;
  const bool wait_for_end_stream_;
  ChainCompleteCallback on_complete_;
  ChainErrorCallback on_error_;
  ChainWatermarkCallback on_high_watermark_;
  ChainWatermarkCallback on_low_watermark_;

  Http::RequestHeaderMapPtr headers_copy_;
  std::unique_ptr<ChainCallbacks> callbacks_;
  std::unique_ptr<ChainFilterManager> filter_manager_;
  std::shared_ptr<CaptureFilter> capture_filter_;
  bool completed_{false};
  bool failed_{false};
  bool destroyed_{false};
};

} // namespace ParallelExtProc
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
