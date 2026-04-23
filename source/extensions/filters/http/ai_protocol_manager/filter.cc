#include "source/extensions/filters/http/ai_protocol_manager/filter.h"

#include "source/common/common/assert.h"
#include "source/common/http/codes.h"
#include "source/common/http/header_map_impl.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_response.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

AiProtocolManagerFilter::AiProtocolManagerFilter(AiProtocolManagerConfigSharedPtr config)
    : config_(std::move(config)),
      payload_store_(config_->decoderConfig().max_inline_bytes),
      decoder_(config_->decoderConfig(), payload_store_) {}

AiProtocolManagerFilter::~AiProtocolManagerFilter() = default;

void AiProtocolManagerFilter::onDestroy() {
  *alive_ = false;
  if (active_chain_) {
    // AiFilterChain destructor calls onDestroy() on each filter.
    active_chain_ = nullptr;
    inference_chain_.reset();
    agentic_chain_.reset();
  }
}

// ── decodeHeaders ─────────────────────────────────────────────────────────────
//
// Analogous to ActiveStream::decodeHeaders() in conn_manager_impl.cc:
//   - Hands headers to the codec (RequestDecoder::onHeaders).
//   - If end_stream: builds the chain and runs it (like the path where
//     filter_manager_.createDownstreamFilterChain() is called immediately).
//   - If body is expected: returns StopIterationAndBuffer so the filter manager
//     buffers data and calls decodeData() / decodeTrailers() later.

Http::FilterHeadersStatus
AiProtocolManagerFilter::decodeHeaders(Http::RequestHeaderMap& headers, bool end_stream) {
  config_->stats().rq_total_.inc();

  auto status = decoder_.onHeaders(headers);
  if (!status.ok()) {
    ENVOY_STREAM_LOG(warn, "ai_protocol_manager: request decode error: {}", *decoder_callbacks_,
                     status.message());
    config_->stats().rq_decode_error_.inc();
    decoder_callbacks_->sendLocalReply(Http::Code::BadRequest,
                                       "AI protocol decode error", nullptr, absl::nullopt, "");
    return Http::FilterHeadersStatus::StopIteration;
  }

  std::cout << "tyxia_Decoder protocol: " << std::endl;

  if (decoder_.protocol() == Codec::ProtocolKind::Unknown) {
    std::cout << "tyxia_Decoder Unknown: " << std::endl;
    non_ai_traffic_ = true;
    return Http::FilterHeadersStatus::Continue;
  }

  std::cout << "tyxia_Decoder_after_protocol: " << std::endl;

  if (end_stream) {
    onEndStreamAndDispatch();
    // In chain-forward mode dispatch() calls continueDecoding() internally;
    // we return StopIteration so the filter manager waits for that call.
    return Http::FilterHeadersStatus::StopIteration;
  }

  if (!decoder_.needsBody()) {
    // Bodiless classification completed in onHeaders (GET, DELETE, etc.).
    onEndStreamAndDispatch();
    return Http::FilterHeadersStatus::StopIteration;
  }

  // Body expected — buffer until decodeData / decodeTrailers.
  return Http::FilterHeadersStatus::StopIteration;
}

// ── decodeData ────────────────────────────────────────────────────────────────

Http::FilterDataStatus
AiProtocolManagerFilter::decodeData(Buffer::Instance& data, bool end_stream) {
  if (non_ai_traffic_) {
    return Http::FilterDataStatus::Continue;
  }
  auto status = decoder_.onData(data.toString());
  if (!status.ok()) {
    ENVOY_STREAM_LOG(warn, "ai_protocol_manager: body decode error: {}", *decoder_callbacks_,
                     status.message());
    config_->stats().rq_decode_error_.inc();
    decoder_callbacks_->sendLocalReply(Http::Code::BadRequest,
                                       "AI protocol body decode error", nullptr, absl::nullopt,
                                       "");
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  if (end_stream) {
    onEndStreamAndDispatch();
  }

  return Http::FilterDataStatus::StopIterationNoBuffer;
}

// ── decodeTrailers ────────────────────────────────────────────────────────────

Http::FilterTrailersStatus
AiProtocolManagerFilter::decodeTrailers(Http::RequestTrailerMap& trailers) {
  if (non_ai_traffic_) {
    return Http::FilterTrailersStatus::Continue;
  }
  auto status = decoder_.onTrailers(trailers);
  if (!status.ok()) {
    ENVOY_STREAM_LOG(warn, "ai_protocol_manager: trailer decode error: {}", *decoder_callbacks_,
                     status.message());
    config_->stats().rq_decode_error_.inc();
  }
  // onTrailers delegates to onEndStream internally, so the chain has already run.
  return Http::FilterTrailersStatus::StopIteration;
}

// ── End-of-stream → chain → dispatch ─────────────────────────────────────────
//
// This is the core of the filter — mirrors what FilterManager::decodeHeaders()
// does after createDownstreamFilterChain():
//   1. Finalize the AiRequest from the decoder.
//   2. Build the appropriate sub-chain (createInferenceChain / createAgenticChain).
//   3. Run chain phase Q1 (onRequestMetadata for all filters).
//   4. Run chain phase Q2 (onRequestItem for each item, for filters that asked).
//   5. Dispatch (continueDecoding in chain-forward mode; AsyncClient in fallout).

void AiProtocolManagerFilter::onEndStreamAndDispatch() {
  auto end_status = decoder_.onEndStream();
  if (!end_status.ok()) {
    ENVOY_STREAM_LOG(warn, "ai_protocol_manager: end stream error: {}", *decoder_callbacks_,
                     end_status.message());
    config_->stats().rq_decode_error_.inc();
    decoder_callbacks_->sendLocalReply(Http::Code::BadRequest, "AI protocol error",
                                       nullptr, absl::nullopt, "");
    return;
  }

  auto take_result = decoder_.take();
  if (!take_result.ok()) {
    ENVOY_STREAM_LOG(warn, "ai_protocol_manager: decoder take error: {}", *decoder_callbacks_,
                     take_result.status().message());
    config_->stats().rq_decode_error_.inc();
    decoder_callbacks_->sendLocalReply(Http::Code::InternalServerError,
                                       "AI protocol internal error", nullptr, absl::nullopt, "");
    return;
  }
  request_ = std::move(take_result.value());

  // Update stats by protocol.
  switch (request_.protocol) {
  case Codec::ProtocolKind::Inference:
    config_->stats().rq_inference_.inc();
    break;
  case Codec::ProtocolKind::AgenticMcp:
  case Codec::ProtocolKind::AgenticA2a:
    config_->stats().rq_agent_.inc();
    break;
  default:
    // config_->stats().rq_classify_unknown_.inc();
    // ENVOY_STREAM_LOG(warn, "ai_protocol_manager: unknown protocol, passing through",
    //                  *decoder_callbacks_);
    // decoder_callbacks_->continueDecoding();
    return;
  }

  // Build the sub-chain — analogous to FilterManager::createDownstreamFilterChain().
  selectAndBuildChain();

  // Run all request-side chain phases, then dispatch.
  runChainRequest();
}

// ── selectAndBuildChain ───────────────────────────────────────────────────────
//
// Creates InferenceChain or AgenticChain from config depending on protocol.
// Analogous to:
//   filter_manager_.createDownstreamFilterChain()
//     → filter_chain_factory_.createFilterChain(callbacks)
//     → for each filter config: callbacks.addStreamDecoderFilter(filter)

void AiProtocolManagerFilter::selectAndBuildChain() {
  switch (request_.protocol) {
  case Codec::ProtocolKind::Inference:
    inference_chain_ = config_->createInferenceChain();
    active_chain_    = inference_chain_.get();
    break;
  case Codec::ProtocolKind::AgenticMcp:
  case Codec::ProtocolKind::AgenticA2a:
    agentic_chain_ = config_->createAgenticChain();
    active_chain_  = agentic_chain_.get();
    break;
  default:
    PANIC("selectAndBuildChain called with unknown protocol");
  }
}

// ── runChainRequest ───────────────────────────────────────────────────────────
//
// Phase Q1 (onRequestMetadata) then Q2 (onRequestItem per item) across the
// active chain. Completion of Q2 (or Q1 if no items are needed) triggers
// dispatch().
//
// This mirrors how FilterManager::decodeHeaders() walks decoder_filters_ and
// then FilterManager::decodeData() walks the same list for body chunks.

void AiProtocolManagerFilter::runChainRequest() {
  ASSERT(active_chain_ != nullptr);

  // Q1 — metadata phase. The on_ready lambda fires when all filters in Q1
  // have returned Continue (immediately for synchronous chains, or after the
  // last continueRequest() for paused ones).
  //
  // on_local_reply fires instead when any filter called sendLocalReply() —
  // the outer filter sends the HTTP response and skips dispatch entirely.
  active_chain_->runRequestMetadata(
      request_, decoder_callbacks_->dispatcher(),
      decoder_callbacks_->streamInfo(), *config_,
      [this](Codec::AiRequest& /*req*/) {
        // Q1 complete. Move to Q2 if any filter declared item interest.
        if (!active_chain_->combinedItemInterest().any()) {
          dispatch();
          return;
        }
        // TODO: materialise each PayloadRef into AiItem, run Q2 phase, re-store.
        dispatch();
      },
      [this](Codec::AiResponse&& resp) {
        // A chain filter (e.g. McpAuthFilter) synthesized a local reply.
        // Send it as an HTTP response and skip the upstream dispatch path.
        ENVOY_STREAM_LOG(debug, "ai_protocol_manager: chain local reply http_status={}",
                         *decoder_callbacks_, resp.http_status);
        decoder_callbacks_->sendLocalReply(
            static_cast<Http::Code>(resp.http_status),
            resp.body, nullptr, absl::nullopt, "");
      });
}

// ── dispatch ──────────────────────────────────────────────────────────────────
//
// Chain-forward mode: re-encode the (possibly mutated) AiRequest back into
// the Envoy header map and call continueDecoding(). The Envoy filter manager
// then runs the remaining HTTP filters and the router filter.
//
// Fallout mode: open an Http::AsyncClient stream to the selected upstream
// cluster (not yet implemented in this skeleton; see §6.2 of DESIGN.md).

void AiProtocolManagerFilter::dispatch() {
  if (dispatched_) {
    return;
  }
  dispatched_ = true;

  // continueDecoding() must not be called synchronously from within a decode
  // callback (FilterManager asserts !(filter_call_state_ & DecodeData)).
  // Post to the next event loop tick to satisfy that invariant.
  auto alive = alive_;
  decoder_callbacks_->dispatcher().post([this, alive = std::move(alive)]() {
    if (!*alive) {
      return;
    }
    doDispatch();
  });
}

void AiProtocolManagerFilter::doDispatch() {
  // TODO: honour config_->dispatchMode() and branch to fallout path.

  switch (request_.protocol) {
  case Codec::ProtocolKind::AgenticMcp:
  case Codec::ProtocolKind::AgenticA2a:
    // Chain-forward: re-encode AgentPayload → JSON-RPC body, mutate headers,
    // inject body into the FM buffer, then resume the decode chain.
    // AgenticDispatch::dispatch() calls continueDecoding() internally.
    Dispatch::AgenticDispatch::dispatch(request_, *decoder_callbacks_);
    return;

  default:
    // Inference and unknown: pass through without re-encoding the body.
    // TODO: invoke RequestEncoder for InferencePayload when body mutations
    //       need to be reflected in the outgoing request.
    ENVOY_STREAM_LOG(debug, "ai_protocol_manager: dispatching via continueDecoding",
                     *decoder_callbacks_);
    decoder_callbacks_->continueDecoding();
    return;
  }
}

// ── Encode path (chain-forward response) ─────────────────────────────────────
//
// In chain-forward mode the upstream response arrives here via the Envoy
// filter manager's encode chain. We feed it through ResponseDecoder (not yet
// implemented) → sub-chain response phases → ResponseEncoder → continueEncoding.
//
// This mirrors how FilterManager::encodeHeaders() / encodeData() walk the
// encoder_filters_ list in reverse order.

Http::FilterHeadersStatus
AiProtocolManagerFilter::encodeHeaders(Http::ResponseHeaderMap& /*headers*/, bool /*end_stream*/) {
  // TODO: feed into ResponseDecoder::onHeaders → run R1 (onResponseStart) across chain.
  // For now: pass through.
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus
AiProtocolManagerFilter::encodeData(Buffer::Instance& /*data*/, bool /*end_stream*/) {
  // TODO: feed into ResponseDecoder::onData → emit AiResponseChunks → run R2 per chunk.
  return Http::FilterDataStatus::Continue;
}

Http::FilterTrailersStatus
AiProtocolManagerFilter::encodeTrailers(Http::ResponseTrailerMap& /*trailers*/) {
  // TODO: feed into ResponseDecoder → run R3 (onResponseEnd) across chain.
  return Http::FilterTrailersStatus::Continue;
}

Http::Filter1xxHeadersStatus AiProtocolManagerFilter::encode1xxHeaders(Http::ResponseHeaderMap& /*headers*/) {
  return Http::Filter1xxHeadersStatus::Continue;
}

Http::FilterMetadataStatus AiProtocolManagerFilter::encodeMetadata(Http::MetadataMap& /*metadata_map*/) {
  return Http::FilterMetadataStatus::Continue;
}

void AiProtocolManagerFilter::onUpstreamHeaders(Http::ResponseHeaderMap& /*headers*/,
                                                bool /*end_stream*/) {}

void AiProtocolManagerFilter::onUpstreamData(Buffer::Instance& /*data*/, bool /*end_stream*/) {}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
