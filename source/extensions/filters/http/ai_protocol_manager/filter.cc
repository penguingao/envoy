#include "source/extensions/filters/http/ai_protocol_manager/filter.h"

#include <limits>
#include <vector>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/http/headers.h"
#include "source/common/http/message_impl.h"
#include "source/common/http/utility.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/gemini_encoder.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/inference_mapping.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/protocol_classifier.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

AiProtocolManagerFilter::AiProtocolManagerFilter(AiProtocolManagerConfigSharedPtr config)
    : config_(config) {}

AiProtocolManagerFilter::~AiProtocolManagerFilter() { cancelActiveRequest(); }

void AiProtocolManagerFilter::onDestroy() { cancelActiveRequest(); }

void AiProtocolManagerFilter::cancelActiveRequest() {
  if (active_request_ != nullptr) {
    active_request_->cancel();
    active_request_ = nullptr;
  }
}

Http::FilterHeadersStatus AiProtocolManagerFilter::decodeHeaders(Http::RequestHeaderMap& headers,
                                                                 bool end_stream) {
  config_->stats().rq_total_.inc();

  // DESIGN.md §4.4 — classify on verb + path + headers. V0 is path-prefix.
  const Codec::ClassifyInput ci{
      headers.getMethodValue(), headers.getPathValue(), headers,
      /*rpc_method=*/"",         config_->classifierPrefixes(),
  };
  const Codec::ClassifyResult cr = Codec::classify(ci);
  protocol_ = cr.protocol;
  classified_ = true;

  switch (protocol_) {
  case Codec::ProtocolKind::Unknown:
    config_->stats().rq_classify_unknown_.inc();
    // Not AI traffic. Pass through — router handles it as a normal HTTP
    // route per the HCM config.
    return Http::FilterHeadersStatus::Continue;
  case Codec::ProtocolKind::Inference:
    config_->stats().rq_inference_.inc();
    break;
  case Codec::ProtocolKind::AgentMcp:
  case Codec::ProtocolKind::AgentA2a:
    config_->stats().rq_agent_.inc();
    // Agent dispatch lands alongside the agent mapper. Until then these
    // requests flow on to the router — operators route them however makes
    // sense for their topology.
    return Http::FilterHeadersStatus::Continue;
  }

  if (!config_->inferenceDispatchConfigured()) {
    // Inference was classified but no dispatch target is configured on this
    // filter instance. Pass through — a downstream route / cluster can
    // still serve it (e.g. proxying to OpenAI without translation).
    return Http::FilterHeadersStatus::Continue;
  }

  // From here on the filter owns the request: it is an Inference call on a
  // route where the operator configured an inference_dispatch target.
  handled_ = true;

  payload_store_ = std::make_unique<Codec::InMemoryPayloadStore>();
  Codec::DecoderConfig dc;
  dc.max_inline_bytes = config_->maxInlineBytes();
  decoder_ = std::make_unique<Codec::AiRequestDecoder>(dc, *payload_store_, protocol_);
  chain_ = std::make_unique<Chain::AiFilterChain>(std::vector<Chain::AiFilterPtr>{});

  // DESIGN.md §4.1 — feed HTTP-level identity into the decoder's internal
  // AiRequest scaffold; keep a non-owning pointer to the header map for
  // sendUpstream reads (Authorization / Host / Content-Type). Filter manager
  // owns `headers` for the stream's lifetime.
  downstream_headers_ = &headers;
  if (auto st = decoder_->onHeaders(headers); !st.ok()) {
    config_->stats().rq_decode_error_.inc();
    decoder_.reset();
  }

  if (end_stream) {
    finalizeRequest();
  }
  return Http::FilterHeadersStatus::StopIteration;
}

Http::FilterDataStatus AiProtocolManagerFilter::decodeData(Buffer::Instance& data, bool end_stream) {
  if (!handled_) {
    return Http::FilterDataStatus::Continue;
  }
  if (decoder_ == nullptr) {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }
  if (data.length() > 0) {
    const uint64_t len64 = data.length();
    if (len64 > std::numeric_limits<uint32_t>::max()) {
      config_->stats().rq_decode_error_.inc();
      decoder_.reset();
    } else {
      const uint32_t len = static_cast<uint32_t>(len64);
      const absl::string_view view(static_cast<const char*>(data.linearize(len)), len);
      auto st = decoder_->onData(view);
      if (!st.ok()) {
        config_->stats().rq_decode_error_.inc();
        decoder_.reset();
      }
    }
  }
  if (end_stream) {
    finalizeRequest();
  }
  return Http::FilterDataStatus::StopIterationNoBuffer;
}

Http::FilterTrailersStatus
AiProtocolManagerFilter::decodeTrailers(Http::RequestTrailerMap& /*trailers*/) {
  if (!handled_) {
    return Http::FilterTrailersStatus::Continue;
  }
  finalizeRequest();
  return Http::FilterTrailersStatus::StopIteration;
}

void AiProtocolManagerFilter::finalizeRequest() {
  if (finalized_ || decoder_ == nullptr) {
    return;
  }
  finalized_ = true;

  auto end_st = decoder_->onEndStream();
  if (!end_st.ok()) {
    config_->stats().rq_decode_error_.inc();
    return;
  }
  auto req_or = decoder_->take();
  if (!req_or.ok()) {
    config_->stats().rq_decode_error_.inc();
    return;
  }
  Codec::AiRequest& req = *req_or;

  // Capture the model + streaming flag from the parsed payload so
  // sendUpstream can build the Vertex URL without re-parsing.
  if (const auto* payload = req.asInference(); payload != nullptr) {
    parsed_model_ = payload->target.name;
    parsed_streaming_ = payload->streaming;
  }

  Chain::UnreachableCallbacks null_cb;
  (void)chain_->runMetadata(req, null_cb);

  // Target schema selects the encoder. OPENAI_PASSTHROUGH re-emits the
  // residual OpenAI body; GEMINI_VERTEX synthesizes a Gemini generateContent
  // body from the parsed payload per OPENAI_VERTEX_SPEC.md §2.
  const auto& dispatch = config_->inferenceDispatch();
  std::unique_ptr<Codec::AiRequestEncoder> encoder;
  switch (dispatch.target_schema) {
  case InferenceDispatchConfig::TargetSchema::GeminiVertex:
    encoder = std::make_unique<Codec::GeminiEncoder>();
    break;
  case InferenceDispatchConfig::TargetSchema::OpenAiPassThrough:
    encoder = std::make_unique<Codec::OpenAiEncoder>();
    break;
  }

  Buffer::OwnedImpl encoded;
  auto enc_st = encoder->encode(req, encoded);
  if (!enc_st.ok()) {
    config_->stats().rq_encode_error_.inc();
    decoder_callbacks_->sendLocalReply(Http::Code::InternalServerError, "encode failed", nullptr,
                                       absl::nullopt, "ai_protocol_manager_encode_error");
    return;
  }
  config_->stats().rq_roundtrip_ok_.inc();

  if (!sendUpstream(encoded)) {
    // Stat already incremented; respond 502.
    decoder_callbacks_->sendLocalReply(Http::Code::BadGateway, "upstream dispatch failed", nullptr,
                                       absl::nullopt, "ai_protocol_manager_dispatch_failed");
  }
}

bool AiProtocolManagerFilter::sendUpstream(const Buffer::Instance& encoded_body) {
  const auto& dispatch = config_->inferenceDispatch();
  auto* cluster = config_->clusterManager().getThreadLocalCluster(dispatch.upstream_cluster);
  if (cluster == nullptr) {
    config_->stats().rq_cluster_not_found_.inc();
    ENVOY_LOG(warn, "ai_protocol_manager: cluster '{}' not found", dispatch.upstream_cluster);
    return false;
  }

  // All downstream-derived request fields are read off the captured header
  // map, not a string snapshot. Headers remain valid: the filter manager
  // owns this map for the whole stream, and we're still inside that stream.
  const Http::RequestHeaderMap* dh = downstream_headers_;
  absl::string_view ds_path = dh != nullptr ? dh->getPathValue() : absl::string_view{};
  absl::string_view ds_host = dh != nullptr ? dh->getHostValue() : absl::string_view{};
  absl::string_view ds_method = dh != nullptr ? dh->getMethodValue() : absl::string_view{};
  absl::string_view ds_content_type = dh != nullptr ? dh->getContentTypeValue()
                                                    : absl::string_view{};
  absl::string_view ds_auth;
  if (dh != nullptr) {
    const auto auth_header = dh->get(Http::CustomHeaders::get().Authorization);
    if (!auth_header.empty()) {
      ds_auth = auth_header[0]->value().getStringView();
    }
  }

  std::string path;
  std::string host = dispatch.upstream_host.empty() ? std::string(ds_host) : dispatch.upstream_host;
  std::string method = "POST";
  std::string content_type = "application/json";

  if (dispatch.target_schema == InferenceDispatchConfig::TargetSchema::GeminiVertex) {
    // OPENAI_VERTEX_SPEC.md §2 — Vertex URL template:
    //   /v1/projects/{project}/locations/{location}/publishers/google/models/{model}:{method}
    //   (+?alt=sse when streaming)
    // model_name_override wins; else the model the client sent.
    const std::string model =
        dispatch.model_name_override.empty() ? parsed_model_ : dispatch.model_name_override;
    if (model.empty() || dispatch.gcp_project.empty() || dispatch.gcp_location.empty()) {
      ENVOY_LOG(warn, "ai_protocol_manager: Vertex dispatch missing model/project/location");
      return false;
    }
    const char* gen_method = parsed_streaming_ ? "streamGenerateContent" : "generateContent";
    path = absl::StrCat("/v1/projects/", dispatch.gcp_project, "/locations/",
                        dispatch.gcp_location, "/publishers/google/models/", model, ":",
                        gen_method);
    if (parsed_streaming_) {
      absl::StrAppend(&path, "?alt=sse");
    }
  } else {
    path = dispatch.upstream_path_override.empty() ? std::string(ds_path)
                                                    : dispatch.upstream_path_override;
    method = ds_method.empty() ? "POST" : std::string(ds_method);
    content_type = ds_content_type.empty() ? std::string(Http::Headers::get().ContentTypeValues.Json)
                                            : std::string(ds_content_type);
  }

  auto headers = Http::createHeaderMap<Http::RequestHeaderMapImpl>({
      {Http::Headers::get().Method, method},
      {Http::Headers::get().Path, path},
      {Http::Headers::get().Host, host},
      {Http::Headers::get().ContentType, content_type},
  });

  // Authorization pass-through. For GCP Vertex the client is responsible for
  // providing an OAuth2 bearer today; proper GCP service-account signing
  // belongs in a companion filter (the existing gcp_authn filter or a
  // dedicated one) and is out of scope for Phase 3a.
  if (!ds_auth.empty()) {
    headers->addReferenceKey(Http::CustomHeaders::get().Authorization, std::string(ds_auth));
  }

  auto message = std::make_unique<Http::RequestMessageImpl>(std::move(headers));
  // Copy the encoded body (we do not own `encoded_body`).
  message->body().add(encoded_body);

  Http::AsyncClient::RequestOptions options;
  options.setTimeout(dispatch.timeout);

  active_request_ = cluster->httpAsyncClient().send(std::move(message), *this, options);
  return active_request_ != nullptr;
}

void AiProtocolManagerFilter::onSuccess(const Http::AsyncClient::Request& /*request*/,
                                        Http::ResponseMessagePtr&& response) {
  active_request_ = nullptr;
  config_->stats().rq_dispatch_ok_.inc();

  // DESIGN.md §2 + §7: response flows through the sub-chain rather than
  // bypassing via decoder_callbacks_->sendLocalReply. Phase 4c implements
  // the non-streaming case — the whole upstream body is wrapped as a
  // single Final chunk. Streaming (SSE per-event chunks) is a later phase
  // that swaps the chunk-emission loop without touching the chain surface.
  const uint64_t status = Http::Utility::getResponseStatus(response->headers());

  // Take ownership of a copy of the upstream headers so the AiResponse
  // pointer remains valid after `response` goes out of scope, and so we
  // can hand the same map to encodeHeaders below.
  Http::ResponseHeaderMapPtr downstream_headers =
      Http::createHeaderMap<Http::ResponseHeaderMapImpl>(response->headers());

  Codec::AiResponse ai_response;
  ai_response.http_status = static_cast<uint32_t>(status);
  ai_response.headers = downstream_headers.get();
  ai_response.protocol = protocol_;
  ai_response.streaming = false;
  ai_response.payload_store = payload_store_.get();

  Chain::UnreachableCallbacks null_cb;

  // R1 — onResponseStart. Filters see scalars + headers but no body yet.
  // Empty chain today; this is the hook point real AiFilters land on.
  if (chain_ != nullptr) {
    (void)chain_->runResponseStart(ai_response, null_cb);
  }

  // R2 — emit a single Final chunk containing the buffered upstream body.
  // For non-streaming responses (the only path Phase 4c implements) the
  // whole body must be available before the chain runs: JSON object key
  // order is unspecified, so a filter that wants to inspect e.g. usage or
  // tool_calls cannot act on a byte prefix without risking forwarding
  // unsafe content. The Final-chunk contract is "complete body, parse and
  // mutate freely; nothing flows downstream until you return."
  std::string content_type;
  if (const auto* ct = downstream_headers->ContentType(); ct != nullptr) {
    content_type = std::string(ct->value().getStringView());
  }
  Codec::ChunkFinalBody final_body;
  final_body.content_type = content_type;
  if (response->body().length() > 0) {
    auto buf = std::make_unique<Buffer::OwnedImpl>();
    buf->add(response->body());
    final_body.body = Codec::PayloadRef::makeBuffered(std::move(buf), Codec::PayloadKind::Other);
  }
  Codec::AiResponseChunk final_chunk = Codec::AiResponseChunk::makeFinal(std::move(final_body));
  if (chain_ != nullptr) {
    (void)chain_->runResponseChunk(final_chunk, null_cb);
  }

  // R3 — onResponseEnd.
  if (chain_ != nullptr) {
    (void)chain_->runResponseEnd(ai_response, null_cb);
  }

  // Forward to downstream via the encode chain. The body bytes come from
  // the (possibly chain-mutated) Final chunk's PayloadRef — never from the
  // original upstream buffer — so any markDirty() rewrite by a chain filter
  // is what the client receives. Per ARCHITECTURE §2 retry contract this is
  // the point of no return: once headers cross encodeHeaders, mid-stream
  // failures cannot trigger re-dispatch.
  Codec::ChunkFinalBody* fb = final_chunk.asFinal();
  const bool has_body = fb != nullptr && fb->body.size() > 0;
  decoder_callbacks_->encodeHeaders(std::move(downstream_headers),
                                    /*end_stream=*/!has_body,
                                    "ai_protocol_manager_dispatch_ok");
  if (has_body) {
    Buffer::OwnedImpl out;
    switch (fb->body.storage()) {
    case Codec::PayloadRef::Storage::Inline:
      out.add(fb->body.inlineView());
      break;
    case Codec::PayloadRef::Storage::Buffered:
      out.add(fb->body.buffered());
      break;
    case Codec::PayloadRef::Storage::External:
      // Async resolution lands with PayloadStore::fetch wiring; for now
      // External should not appear here because the chain runs synchronously.
      ENVOY_LOG(error, "ai_protocol_manager: External Final body not yet supported");
      break;
    }
    decoder_callbacks_->encodeData(out, /*end_stream=*/true);
  }
}

void AiProtocolManagerFilter::onFailure(const Http::AsyncClient::Request& /*request*/,
                                        Http::AsyncClient::FailureReason reason) {
  active_request_ = nullptr;
  config_->stats().rq_dispatch_failure_.inc();
  const char* reason_str = "unknown";
  switch (reason) {
  case Http::AsyncClient::FailureReason::Reset:
    reason_str = "upstream reset";
    break;
  case Http::AsyncClient::FailureReason::ExceedResponseBufferLimit:
    reason_str = "response buffer limit exceeded";
    break;
  }
  ENVOY_LOG(warn, "ai_protocol_manager: upstream failure ({})", reason_str);
  decoder_callbacks_->sendLocalReply(Http::Code::BadGateway, reason_str, nullptr, absl::nullopt,
                                     "ai_protocol_manager_dispatch_failed");
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
