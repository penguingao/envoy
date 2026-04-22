#include "source/extensions/filters/http/ai_protocol_manager/codec/protocol_classifier.h"

#include "absl/strings/match.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

ClassifyResult classify(const ClassifyInput& input) {
  ClassifyResult result;
  for (const auto& pfx : input.prefixes.inference_prefixes) {
    if (absl::StartsWith(input.path, pfx)) {
      result.protocol = ProtocolKind::Inference;
      // Invocation disambiguation (ChatCompletion vs Responses vs resource
      // ops) lives alongside the path-template extractor in the phase that
      // adds Responses resource ops. For now the mapper still derives
      // invocation from the body.
      return result;
    }
  }
  for (const auto& pfx : input.prefixes.agent_prefixes) {
    if (absl::StartsWith(input.path, pfx)) {
      // Dialect discrimination (A2a vs Mcp) lives in the agent mapper in V1;
      // for the classifier's purposes Mcp is the safe default since it is
      // the JSON-RPC-native dialect.
      result.protocol = ProtocolKind::AgentMcp;
      return result;
    }
  }
  return result;
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
