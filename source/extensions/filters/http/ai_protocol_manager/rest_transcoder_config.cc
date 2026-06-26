#include "source/extensions/filters/http/ai_protocol_manager/rest_transcoder_config.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

McpRestTranscoderRouteConfig::McpRestTranscoderRouteConfig(
    const McpRestTranscoderConfigProto& proto) {
  for (const auto& entry : proto.tools()) {
    if (!entry.tool_name().empty()) {
      tools_[entry.tool_name()] = entry.http_rule();
    }
  }
  if (proto.has_tools_list_rule()) {
    tools_list_rule_ = proto.tools_list_rule();
  }
  if (proto.has_resources_list_rule()) {
    resources_list_rule_ = proto.resources_list_rule();
  }
  if (proto.has_resources_read_rule()) {
    resources_read_rule_ = proto.resources_read_rule();
  }
}

const HttpRuleProto* McpRestTranscoderRouteConfig::toolRule(absl::string_view tool_name) const {
  auto it = tools_.find(tool_name);
  return it != tools_.end() ? &it->second : nullptr;
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
