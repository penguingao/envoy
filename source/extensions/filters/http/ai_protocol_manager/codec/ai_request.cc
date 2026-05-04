#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

std::string materializeRef(const PayloadRef& ref, const AiRequest& request) {
  if (ref.storage() != PayloadRef::Storage::External) {
    return ref.toString();
  }
  ASSERT(request.payload_store != nullptr);
  std::string result;
  if (request.payload_store != nullptr) {
    request.payload_store->fetch(ref, [&](Buffer::InstancePtr buf) {
      if (buf) {
        result = buf->toString();
      }
    });
  }
  return result;
}

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
