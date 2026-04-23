#include "source/extensions/filters/http/ai_protocol_manager/dispatch/agentic_dispatch.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/request_encoder.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Dispatch {

void AgenticDispatch::dispatch(Codec::AiRequest& request,
                               Http::StreamDecoderFilterCallbacks& callbacks) {
  ASSERT(request.protocol == Codec::ProtocolKind::AgenticMcp ||
         request.protocol == Codec::ProtocolKind::AgenticA2a);

  // ── Step 1: Re-encode AgentPayload → JSON-RPC body ───────────────────────
  //
  // For structured invocations (ToolsCall, Resources*, PromptsGet), the body
  // reflects chain-filter mutations to the AgentPayload fields.
  // For all other invocations, params_raw provides a faithful pass-through of
  // fields that were not extracted into structured form.
  const std::string body_str = Codec::RequestEncoder::encodeAgentBody(request);
  const uint64_t content_length = static_cast<uint64_t>(body_str.size());

  ENVOY_LOG(debug,
            "ai_protocol_manager: agentic chain-forward dispatch "
            "method={} path={} invocation={} body_bytes={}",
            request.http_method, request.path,
            static_cast<int>(request.as_agent() ? request.as_agent()->invocation
                                                 : Codec::AgentInvocation::Unknown),
            content_length);

  // ── Step 2: Mutate the downstream request header map in place ────────────
  //
  // requestHeaders() is the authoritative handle — use it rather than the
  // non-owning request.headers pointer to ensure the filter manager sees the
  // updated values when it resumes the decode chain.
  auto headers_opt = callbacks.requestHeaders();
  if (headers_opt.has_value()) {
    // :method — typically POST for JSON-RPC; routing filters may have changed it.
    headers_opt->setMethod(request.http_method);
    // :path — routing filters may have rewritten this to target a different agent.
    headers_opt->setPath(request.path);
    // Ensure downstream sees JSON content-type regardless of the original value.
    headers_opt->setContentType("application/json");
    // Update content-length to match the re-encoded body.
    headers_opt->setContentLength(content_length);
  }

  // ── Step 3: Inject the re-encoded body into the filter manager ───────────
  //
  // Since AiProtocolManagerFilter returned StopIterationNoBuffer from
  // decodeData, the filter manager did not buffer the original body.
  // addDecodedData places our re-encoded body into the FM buffer so that
  // subsequent decoder filters and the router receive the correct payload when
  // continueDecoding() resumes the chain.
  //
  // streaming=false: we have the complete body; no further chunks are expected.
  Buffer::OwnedImpl new_body(body_str);
  callbacks.addDecodedData(new_body, /*streaming_filter=*/false);

  // ── Step 4: Resume the decode chain ──────────────────────────────────────
  //
  // The filter manager resumes decodeHeaders on subsequent filters, then
  // dispatches the buffered body (our re-encoded JSON-RPC) as decodeData,
  // and finally signals end_stream.
  callbacks.continueDecoding();
}

} // namespace Dispatch
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
