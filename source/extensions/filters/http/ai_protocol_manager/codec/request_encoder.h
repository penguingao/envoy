#pragma once

#include <string>

#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

// RequestEncoder translates a (possibly mutated) AiRequest back into an
// outbound HTTP body. It is the inverse of RequestDecoder.
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
};

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
