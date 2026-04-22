#pragma once

#include <memory>
#include <string>

#include "envoy/buffer/buffer.h"
#include "envoy/http/async_client.h"
#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"

#include "source/common/common/logger.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter_chain.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain/inference_chain.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_payload.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request_decoder.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_response.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_response_chunk.h"
#include "source/extensions/filters/http/ai_protocol_manager/filter_config.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// DESIGN.md §2 + §7 — decoder-only HTTP filter, non-terminal.
//
// Lifecycle:
//   decodeHeaders  → classify; for Inference-classified requests with a
//                    dispatch target configured, feed HTTP-level fields
//                    into the decoder and StopIteration — this request
//                    will be handled by this filter. Every other path
//                    (Unknown, Agent-not-yet-implemented, no dispatch
//                    configured) returns Continue so the router handles
//                    the request normally.
//   decodeData     → feed decoder accumulator for handled requests; for
//                    the Continue path decodeData is a pure pass-through.
//   decodeTrailers → finalizeRequest on handled requests; Continue
//                    otherwise.
//   onSuccess      → build AiResponse, run R1/R2(Final)/R3 through the
//                    chain, forward to downstream via
//                    decoder_callbacks_->encodeHeaders / encodeData.
//   onFailure      → sendLocalReply with a synthesized upstream-error body.
//
// Non-terminal on purpose: a proxy typically carries both AI and non-AI
// traffic on the same listener, and per-route config decides whether
// the filter engages (ARCHITECTURE.md §2 / future per-route override).
// The filter is terminal for the REQUESTS it takes ownership of (those
// never reach the router), but the chain must contain a terminal filter
// downstream (typically envoy.filters.http.router) for the requests the
// filter passes through.
class AiProtocolManagerFilter : public Http::PassThroughFilter,
                                public Http::AsyncClient::Callbacks,
                                public Logger::Loggable<Logger::Id::filter> {
public:
  explicit AiProtocolManagerFilter(AiProtocolManagerConfigSharedPtr config);
  ~AiProtocolManagerFilter() override;

  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;
  Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap& trailers) override;
  void onDestroy() override;

  // Http::AsyncClient::Callbacks
  void onSuccess(const Http::AsyncClient::Request& request,
                 Http::ResponseMessagePtr&& response) override;
  void onFailure(const Http::AsyncClient::Request& request,
                 Http::AsyncClient::FailureReason reason) override;
  void onBeforeFinalizeUpstreamSpan(Tracing::Span&,
                                    const Http::ResponseHeaderMap*) override {}

private:
  // True when this filter has taken ownership of the request (classified
  // Inference AND an inference_dispatch cluster is configured). False for
  // every "pass through to router" path: Unknown classification, agent
  // traffic until the agent mapper lands, or Inference without a dispatch
  // config. Determines StopIteration vs Continue throughout the request.
  bool handled_{false};

  // Build and send the outbound request. Returns false on error (stats
  // already incremented by the time we return false).
  bool sendUpstream(const Buffer::Instance& encoded_body);

  // Finalize the request pipeline: decode → chain → encode → dispatch. Called
  // from decodeData(end_stream=true) or decodeTrailers().
  void finalizeRequest();

  void cancelActiveRequest();

  AiProtocolManagerConfigSharedPtr config_;

  // Per-stream state.
  std::unique_ptr<Codec::PayloadStore> payload_store_;
  std::unique_ptr<Codec::AiRequestDecoder> decoder_;
  Chain::AiFilterChainPtr chain_;
  Codec::ProtocolKind protocol_{Codec::ProtocolKind::Unknown};
  bool classified_{false};
  bool finalized_{false};

  // Non-owning view of the downstream request headers captured in
  // decodeHeaders. Valid for the lifetime of the stream; sendUpstream reads
  // :authority / Authorization / Content-Type off it instead of snapshotting
  // strings. Same pointer also lives on AiRequest::headers after take().
  Http::RequestHeaderMap* downstream_headers_{nullptr};

  // Parsed during finalizeRequest, consumed by sendUpstream — needed for
  // GeminiVertex URL construction.
  std::string parsed_model_;
  bool parsed_streaming_{false};

  // AsyncClient state. active_request_ is held so we can cancel on destroy.
  Http::AsyncClient::Request* active_request_{nullptr};
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
