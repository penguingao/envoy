#pragma once

#include "envoy/router/router.h"

#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.h"

#include "absl/container/flat_hash_map.h"
#include "absl/types/optional.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

using McpRestTranscoderConfigProto =
    envoy::extensions::filters::http::ai_protocol_manager::v3::McpRestTranscoderConfig;
using HttpRuleProto = envoy::extensions::filters::http::ai_protocol_manager::v3::HttpRule;

// Per-route REST transcoder config, parsed from typed_per_filter_config at RDS load time.
//
// When resolved at dispatch time via resolveMostSpecificPerFilterConfig,
// AgenticDispatch branches from JSON-RPC re-encoding to REST lowering using
// RequestEncoder::encodeAgentBodyAsRest().
class McpRestTranscoderRouteConfig : public Router::RouteSpecificFilterConfig {
public:
  explicit McpRestTranscoderRouteConfig(const McpRestTranscoderConfigProto& proto);

  // Returns the HttpRule for tools/call for the given tool name, or nullptr if not configured.
  const HttpRuleProto* toolRule(absl::string_view tool_name) const;

  // Returns the HttpRule for tools/list, or nullptr if not configured.
  const HttpRuleProto* toolsListRule() const {
    return tools_list_rule_.has_value() ? &*tools_list_rule_ : nullptr;
  }

  // Returns the HttpRule for resources/list, or nullptr if not configured.
  const HttpRuleProto* resourcesListRule() const {
    return resources_list_rule_.has_value() ? &*resources_list_rule_ : nullptr;
  }

  // Returns the HttpRule for resources/read, or nullptr if not configured.
  // The {uri} template variable is substituted with AgentPayload::resource_uri.
  const HttpRuleProto* resourcesReadRule() const {
    return resources_read_rule_.has_value() ? &*resources_read_rule_ : nullptr;
  }

private:
  absl::flat_hash_map<std::string, HttpRuleProto> tools_;
  absl::optional<HttpRuleProto> tools_list_rule_;
  absl::optional<HttpRuleProto> resources_list_rule_;
  absl::optional<HttpRuleProto> resources_read_rule_;
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
