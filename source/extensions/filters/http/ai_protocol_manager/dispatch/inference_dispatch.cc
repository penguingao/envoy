#include "source/extensions/filters/http/ai_protocol_manager/dispatch/inference_dispatch.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/request_encoder.h"
#include "source/extensions/filters/http/ai_protocol_manager/providers/anthropic/anthropic_request_encoder.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Dispatch {

void InferenceDispatch::dispatch(Codec::AiRequest& request,
                                 Http::StreamDecoderFilterCallbacks& callbacks) {
  ASSERT(request.protocol == Codec::ProtocolKind::Inference);

  // ── Step 1: Encode body ───────────────────────────────────────────────────
  //
  // Try a provider-specific encoder first; fall back to the OpenAI round-trip
  // encoder when no provider match is found or the encoder returns nullopt
  // (e.g. unsupported invocation type for that provider).
  //
  // The provider encoder may also rewrite :method and :path (e.g. Anthropic
  // uses POST /v1/messages instead of POST /v1/chat/completions), so we track
  // effective_method / effective_path separately from request.http_method /
  // request.path which hold the original downstream values.
  std::string body_str;
  std::string effective_method = request.http_method;
  std::string effective_path = request.path;

  const Codec::InferencePayload* payload = request.as_inference();
  if (payload != nullptr && payload->target.provider_hint == "anthropic") {
    auto result = Providers::AnthropicRequestEncoder::encode(request);
    if (result.has_value()) {
      effective_method = std::move(result->method);
      effective_path = std::move(result->path);
      body_str = std::move(result->body);
    }
  }

  // Fall back to OpenAI-compatible round-trip encoding when the provider
  // encoder was not selected or returned nullopt.
  if (body_str.empty() && effective_path == request.path) {
    body_str = Codec::RequestEncoder::encodeInferenceBody(request);
  }

  const bool is_bodied = !body_str.empty();
  const uint64_t content_length = static_cast<uint64_t>(body_str.size());

  ENVOY_LOG(debug,
            "ai_protocol_manager: inference chain-forward dispatch "
            "provider={} method={} path={} invocation={} body_bytes={}",
            payload ? payload->target.provider_hint : "",
            effective_method, effective_path,
            static_cast<int>(payload ? payload->invocation
                                     : Codec::InferenceInvocation::Unknown),
            content_length);

  // ── Step 2: Mutate the downstream request header map in place ────────────
  //
  // Use effective_method / effective_path rather than request.http_method /
  // request.path so provider rewrites (e.g. /v1/messages) are applied.
  auto headers_opt = callbacks.requestHeaders();
  if (headers_opt.has_value()) {
    headers_opt->setMethod(effective_method);
    headers_opt->setPath(effective_path);
    if (is_bodied) {
      headers_opt->setContentType("application/json");
      headers_opt->setContentLength(content_length);
    }
  }

  // ── Step 3: Inject re-encoded body for bodied requests ───────────────────
  if (is_bodied) {
    Buffer::OwnedImpl new_body(body_str);
    callbacks.addDecodedData(new_body, /*streaming_filter=*/false);
  }

  // ── Step 4: Resume the decode chain ──────────────────────────────────────
  callbacks.continueDecoding();
}

} // namespace Dispatch
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
