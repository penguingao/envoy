#pragma once

#include <string>

#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"
#include "source/extensions/filters/http/ai_protocol_manager/rest_transcoder_config.h"

#include "absl/types/optional.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

struct RestHttpRequest {
  std::string method;
  std::string path;
  std::string body;
};

// RequestEncoder translates a (possibly mutated) AiRequest back into an
// outbound HTTP request. It is the inverse of RequestDecoder.
//
// Chain-forward dispatch sequence (DESIGN.md §6.3):
//   RequestEncoder → body string → AgenticDispatch mutates headers + body →
//   decoder_callbacks_->continueDecoding()
//
// Mutation visibility:
//   - Structured fields mutated by chain filters (tool_name, resource_uri,
//     prompt_name, arguments) are always reflected in the encoded body.
//   - Fields not extracted into structured form (Initialize params beyond
//     capabilities, A2A message parts, list cursors, …) are replayed from
//     params_raw and will NOT reflect any mutations made directly to
//     AgentPayload::residual_params.
class RequestEncoder {
public:
  // Encodes the AgentPayload of the AiRequest back into a JSON-RPC body.
  //
  // For invocations whose params are fully modelled (ToolsCall, Resources*,
  // PromptsGet), params are reconstructed from structured fields so that
  // chain-filter mutations (e.g. tool_name rewrite) appear in the body.
  //
  // For all other invocations (Initialize, Ping, ToolsList, A2A ops, …),
  // params_raw is used verbatim to guarantee a faithful round-trip for the
  // fields we did not extract.
  //
  // `request` must have protocol == AgenticMcp or AgenticA2a and a populated
  // AgentPayload; returns an empty string on contract violation.
  static std::string encodeAgentBody(const AiRequest& request);

  // Lowers the AgentPayload to a plain REST HTTP request using the per-route
  // transcoder config.  The AgentPayload structured fields (tool_name,
  // resource_uri, arguments) are used directly — no re-parsing of the original
  // body is required.
  //
  // Returns nullopt when:
  //   - No matching HttpRule is found in `transcoder` for this invocation.
  //   - The arguments JSON is malformed.
  //   - The HttpRule has no HTTP method set.
  static absl::optional<RestHttpRequest> encodeAgentBodyAsRest(
      const AiRequest& request, const McpRestTranscoderRouteConfig& transcoder);

  // Encodes the InferencePayload of the AiRequest back into an OpenAI-style
  // JSON body string.
  //
  // Strategy: start from residual_params (the full original body) as the base,
  // then overlay any extracted fields with their current (possibly mutated by
  // chain filters) values. messages[] and tools[] are rebuilt from their
  // PayloadRefs so per-item mutations from Q2 onRequestItem are reflected.
  //
  // Returns an empty string for bodiless invocations (ResponsesRetrieve,
  // ResponsesCancel, ResponsesDelete, ResponsesListInputItems) — InferenceDispatch
  // skips addDecodedData / content-length mutation in that case.
  //
  // `request` must have protocol == Inference and a populated InferencePayload;
  // returns an empty string on contract violation.
  static std::string encodeInferenceBody(const AiRequest& request);
};

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
