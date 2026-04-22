#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_response.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

InferenceResponseSummary* AiResponse::asInference() {
  return std::get_if<InferenceResponseSummary>(&summary);
}
const InferenceResponseSummary* AiResponse::asInference() const {
  return std::get_if<InferenceResponseSummary>(&summary);
}

AgentResponseSummary* AiResponse::asAgent() {
  return std::get_if<AgentResponseSummary>(&summary);
}
const AgentResponseSummary* AiResponse::asAgent() const {
  return std::get_if<AgentResponseSummary>(&summary);
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
