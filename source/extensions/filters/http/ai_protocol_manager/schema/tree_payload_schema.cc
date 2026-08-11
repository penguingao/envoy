#include "source/extensions/filters/http/ai_protocol_manager/schema/tree_payload_schema.h"

#include "source/extensions/filters/http/ai_protocol_manager/schema/schema_validator.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

absl::Status TreePayloadSchema::validate(const nlohmann::json& payload) const {
  return AiProtocolManager::validate(payload, *root_);
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
