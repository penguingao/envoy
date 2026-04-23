#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

InferencePayload* AiRequest::as_inference() {
  return std::get_if<InferencePayload>(&payload);
}

const InferencePayload* AiRequest::as_inference() const {
  return std::get_if<InferencePayload>(&payload);
}

AgentPayload* AiRequest::as_agent() {
  return std::get_if<AgentPayload>(&payload);
}

const AgentPayload* AiRequest::as_agent() const {
  return std::get_if<AgentPayload>(&payload);
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
