#pragma once

#include <memory>
#include <atomic>

#include "envoy/http/filter.h"

#include "source/common/common/logger.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain/agentic_chain.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain/inference_chain.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_payload.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/request_decoder.h"
#include "source/extensions/filters/http/ai_protocol_manager/dispatch/agentic_dispatch.h"
#include "source/extensions/filters/http/ai_protocol_manager/dispatch/inference_dispatch.h"
#include "source/extensions/filters/http/ai_protocol_manager/filter_config.h"
#include "source/extensions/filters/http/ai_protocol_manager/rest_transcoder_config.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// AiProtocolManagerFilter — the outer Envoy HTTP filter.
//
// Lifecycle analogy to HCM / FilterManager
// ─────────────────────────────────────────
//  ConnectionManagerImpl::onData()
//    → codec_->dispatch()
//    → ActiveStream::decodeHeaders()
//       → filter_manager_.createDownstreamFilterChain()   ← one call per stream
//       → filter_manager_.decodeHeaders()                 ← walks decoder_filters_
//
//  AiProtocolManagerFilter::decodeHeaders()
//    → decoder_.onHeaders()                               ← classify, pick body parser
//    → if end_stream: onEndStreamAndDispatch()
//         → decoder_.onEndStream()                        ← finalize AiRequest
//         → selectAndBuildChain()                         ← create InferenceChain or AgenticChain
//         → active_chain_->runRequestMetadata(req, ...)   ← walk chain phase Q1
//         → active_chain_->runRequestItem(...)            ← walk chain phase Q2 per item
//         → dispatch()                                    ← continueDecoding() or AsyncClient
//
// Implements StreamDecoderFilter + StreamEncoderFilter so it handles both the
// request path (decode*) and the response path (encode*) in chain-forward mode.
// In fallout mode the encode side is unused (AsyncClient owns the response).
class AiProtocolManagerFilter : public Http::StreamFilter,
                                 public Logger::Loggable<Logger::Id::filter> {
public:
  enum class DispatchResult { Error, NonAiTraffic, AiTraffic };
  explicit AiProtocolManagerFilter(AiProtocolManagerConfigSharedPtr config);
  ~AiProtocolManagerFilter() override;

  // ── Http::StreamDecoderFilter ────────────────────────────────────────────

  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus    decodeData(Buffer::Instance& data, bool end_stream) override;
  Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap& trailers) override;

  void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override {
    decoder_callbacks_ = &callbacks;
  }
  void onDestroy() override;

  // ── Http::StreamEncoderFilter (chain-forward mode) ───────────────────────
  // In fallout mode these return Continue immediately — the AsyncClient owns
  // the response path, not the Envoy encode chain.

  Http::FilterHeadersStatus  encodeHeaders(Http::ResponseHeaderMap& headers,
                                           bool end_stream) override;
  Http::FilterDataStatus     encodeData(Buffer::Instance& data, bool end_stream) override;
  Http::FilterTrailersStatus encodeTrailers(Http::ResponseTrailerMap& trailers) override;

  Http::Filter1xxHeadersStatus encode1xxHeaders(Http::ResponseHeaderMap& headers) override;
  Http::FilterMetadataStatus encodeMetadata(Http::MetadataMap& metadata_map) override;

  void setEncoderFilterCallbacks(Http::StreamEncoderFilterCallbacks& callbacks) override {
    encoder_callbacks_ = &callbacks;
  }

private:
  // ── Internal flow ─────────────────────────────────────────────────────────

  // Called once decodeHeaders has arrived (and potentially decodeData /
  // decodeTrailers — when end_stream is set). Finalises RequestDecoder,
  // builds the appropriate sub-chain, runs all request-side chain phases,
  // then triggers dispatch.
  DispatchResult onEndStreamAndDispatch();

  // Creates InferenceChain or AgenticChain from config depending on the
  // classified protocol in request_.protocol. Analogous to
  // FilterManager::createDownstreamFilterChain().
  void selectAndBuildChain();

  // Run Q1 (onRequestMetadata) then Q2 (onRequestItem for each item) across
  // active_chain_. When all phases complete, calls dispatch().
  void runChainRequest();

  // Dispatch — the tail of the request-side flow.
  // Posts doDispatch() to the next event loop tick to avoid calling
  // continueDecoding() from within a decode callback.
  void dispatch();

  // Actual dispatch logic, called from dispatch() via dispatcher().post().
  void doDispatch();

  // ── Chain-forward response path ──────────────────────────────────────────

  // encodeHeaders/Data/Trailers feed the ResponseDecoder (not yet implemented),
  // which produces AiResponse + AiResponseChunk stream.  The chain's response
  // phases (R1/R2/R3) run over those objects before forwarding downstream.
  void onUpstreamHeaders(Http::ResponseHeaderMap& headers, bool end_stream);
  void onUpstreamData(Buffer::Instance& data, bool end_stream);

  // ── State ─────────────────────────────────────────────────────────────────

  AiProtocolManagerConfigSharedPtr config_;

  Http::StreamDecoderFilterCallbacks* decoder_callbacks_{nullptr};
  Http::StreamEncoderFilterCallbacks* encoder_callbacks_{nullptr};

  // Per-stream PayloadStore and RequestDecoder.
  Codec::InMemoryPayloadStore payload_store_;
  Codec::RequestDecoder       decoder_;

  // Populated after onEndStream.
  Codec::AiRequest request_;

  // The active sub-chain — set by selectAndBuildChain(), owned for stream lifetime.
  // Exactly one of inference_chain_ or agentic_chain_ is non-null after
  // selectAndBuildChain().
  Chain::InferenceChainPtr inference_chain_;
  Chain::AgenticChainPtr   agentic_chain_;

  // Non-owning pointer to whichever chain is active (avoids virtual dispatch
  // through the two unique_ptrs for the common run*() calls).
  Chain::AiFilterChain* active_chain_{nullptr};

  // True in chain-forward mode once decode side has called continueDecoding().
  bool dispatched_{false};

  // Shared flag set to false in onDestroy() so any posted dispatcher lambdas
  // can bail out safely if the stream is reset before they run.
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};

  bool non_ai_traffic_{false};

  const McpRestTranscoderRouteConfig* transcoder_config_{nullptr};
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
