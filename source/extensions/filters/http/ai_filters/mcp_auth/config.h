#pragma once

#include <memory>
#include <string>
#include <vector>

#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"

#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter_factory.h"

#include "envoy/extensions/filters/http/ai_filters/mcp_auth/v3/mcp_auth.pb.h"
#include "envoy/extensions/filters/http/ai_filters/mcp_auth/v3/mcp_auth.pb.validate.h"

#include "absl/container/flat_hash_set.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"

#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiFilters {
namespace McpAuth {

// String matcher — one of: exact, prefix, suffix.
struct StringMatcher {
  enum class Kind { Exact, Prefix, Suffix };
  Kind        kind{Kind::Exact};
  std::string value;

  bool matches(absl::string_view s) const {
    switch (kind) {
    case Kind::Exact:  return s == value;
    case Kind::Prefix: return absl::StartsWith(s, value);
    case Kind::Suffix: return absl::EndsWith(s, value);
    }
    return false;
  }
};

// Which MCP param field a ParamCondition inspects.
enum class ParamField {
  ToolName,    // AgentPayload.tool_name    (tools/call)
  ResourceUri, // AgentPayload.resource_uri (resources/*)
  PromptName,  // AgentPayload.prompt_name  (prompts/get)
};

// A single condition on one MCP param field.
struct ParamCondition {
  ParamField    field;
  StringMatcher matcher;

  // Returns true when the condition holds for the given agent payload.
  // Returns false when payload is nullptr (param not available).
  bool evaluate(const AiProtocolManager::Codec::AgentPayload* agent) const {
    if (agent == nullptr) {
      return false;
    }
    switch (field) {
    case ParamField::ToolName:    return matcher.matches(agent->tool_name);
    case ParamField::ResourceUri: return matcher.matches(agent->resource_uri);
    case ParamField::PromptName:  return matcher.matches(agent->prompt_name);
    }
    return false;
  }
};

// A single entry in the method policy list.
//
// A policy matches a request when:
//   1. method_pattern matches the JSON-RPC method name, AND
//   2. every param_condition holds against the AgentPayload.
//
// allowed_principals controls who may call matched methods;
// "*" means any authenticated principal; empty means deny all.
struct MethodPolicy {
  std::string                      method_pattern;
  absl::flat_hash_set<std::string> allowed_principals;
  std::vector<ParamCondition>      param_conditions;

  // Returns true when method and all param conditions match.
  bool matches(absl::string_view method,
               const AiProtocolManager::Codec::AgentPayload* agent) const {
    // Method pattern check.
    if (!method_pattern.empty() && method_pattern.back() == '*') {
      if (!absl::StartsWith(method, absl::string_view(method_pattern)
                                        .substr(0, method_pattern.size() - 1))) {
        return false;
      }
    } else if (method != method_pattern) {
      return false;
    }
    // All param conditions must hold.
    for (const auto& cond : param_conditions) {
      if (!cond.evaluate(agent)) {
        return false;
      }
    }
    return true;
  }

  // Returns true when the given principal is permitted by this policy.
  bool allows(absl::string_view principal) const {
    return allowed_principals.contains("*") || allowed_principals.contains(principal);
  }
};

// McpAuthFilterConfig — the parsed, immutable per-listener configuration.
//
// Shared (shared_ptr, const) across all streams on a listener worker.
// Populated from McpAuthConfig proto at filter-chain construction time.
struct McpAuthFilterConfig {
  // Name of the HTTP header that carries the session identity (JWT sub,
  // API key, OAuth2 client_id, …). Default: "x-mcp-identity".
  std::string identity_header{"x-mcp-identity"};

  // JSON-RPC methods that bypass authentication entirely.
  // "initialize" is always present regardless of configuration.
  absl::flat_hash_set<std::string> allowed_unauthenticated_methods;

  // Deprecated: used only when method_policies is empty.
  // Methods whose name starts with this prefix are restricted to "admin".
  std::string admin_method_prefix{"admin/"};

  // Per-method access policies loaded from the proto, evaluated in order.
  // When non-empty, these replace admin_method_prefix entirely.
  std::vector<MethodPolicy> method_policies;

  // Convenience constructor that guarantees "initialize" is always allowed.
  McpAuthFilterConfig() { allowed_unauthenticated_methods.insert("initialize"); }
};

using McpAuthFilterConfigSharedPtr = std::shared_ptr<const McpAuthFilterConfig>;

// McpAuthFilterFactory — creates McpAuthFilter instances for each new stream.
//
// Registration name: "envoy.ai_filters.mcp_auth"
// Registry: AiProtocolManager::Chain::AiFilterFactory (looked up by buildChain).
// Proto: envoy.extensions.filters.http.ai_filters.mcp_auth.v3.McpAuthConfig
class McpAuthFilterFactory : public AiProtocolManager::Chain::AiFilterFactory {
public:
  // ── AiFilterFactory ───────────────────────────────────────────────────────

  std::string name() const override { return "envoy.ai_filters.mcp_auth"; }

  void createAiFilter(AiProtocolManager::Chain::AiFilterChainCallbacks& callbacks,
                      const Protobuf::Any& typed_config,
                      Server::Configuration::FactoryContext& context) override;

  ProtobufTypes::MessagePtr createEmptyConfigProto() override;

private:
  static McpAuthFilterConfigSharedPtr parseConfig(const Protobuf::Any& typed_config,
                                                  Server::Configuration::FactoryContext& context);
};

} // namespace McpAuth
} // namespace AiFilters
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
