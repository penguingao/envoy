#include "source/extensions/filters/http/ai_filters/mcp_auth/config.h"

#include "envoy/registry/registry.h"

#include "source/common/protobuf/utility.h"
#include "source/common/protobuf/message_validator_impl.h"
#include "source/extensions/filters/http/ai_filters/mcp_auth/filter.h"

#include "envoy/extensions/filters/http/ai_filters/mcp_auth/v3/mcp_auth.pb.h"
#include "envoy/extensions/filters/http/ai_filters/mcp_auth/v3/mcp_auth.pb.validate.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiFilters {
namespace McpAuth {

using ProtoConfig = envoy::extensions::filters::http::ai_filters::mcp_auth::v3::McpAuthConfig;

// ── createEmptyConfigProto ────────────────────────────────────────────────────

ProtobufTypes::MessagePtr McpAuthFilterFactory::createEmptyConfigProto() {
  return std::make_unique<ProtoConfig>();
}

// ── parseConfig ───────────────────────────────────────────────────────────────
//
// Unpacks the Any-packed McpAuthConfig proto and maps its fields into the
// immutable McpAuthFilterConfig that is shared across all streams.
//
// Field defaults:
//   identity_header      → "x-mcp-identity"
//   admin_method_prefix  → "admin/" (only used when method_policies is empty)
//   "initialize" is always inserted into allowed_unauthenticated_methods.
//
// When method_policies is non-empty it supersedes admin_method_prefix.

McpAuthFilterConfigSharedPtr
McpAuthFilterFactory::parseConfig(const Protobuf::Any& typed_config,
                                   Server::Configuration::FactoryContext& /*context*/) {
  ProtoConfig proto;
  MessageUtil::anyConvertAndValidate<ProtoConfig>(typed_config, proto,
                                                  ProtobufMessage::getStrictValidationVisitor());

  auto cfg = std::make_shared<McpAuthFilterConfig>();

  // identity_header — default "x-mcp-identity" set by McpAuthFilterConfig ctor.
  if (!proto.identity_header().empty()) {
    cfg->identity_header = proto.identity_header();
  }

  // allowed_unauthenticated_methods — "initialize" always present (added by ctor).
  for (const auto& method : proto.allowed_unauthenticated_methods()) {
    cfg->allowed_unauthenticated_methods.insert(method);
  }

  // method_policies — when present, supersedes admin_method_prefix.
  using ProtoField = envoy::extensions::filters::http::ai_filters::mcp_auth::v3::ParamCondition;
  for (const auto& p : proto.method_policies()) {
    MethodPolicy policy;
    policy.method_pattern = p.method_pattern();
    for (const auto& principal : p.allowed_principals()) {
      policy.allowed_principals.insert(principal);
    }

    for (const auto& pc : p.param_conditions()) {
      ParamCondition cond;

      switch (pc.field()) {
      case ProtoField::TOOL_NAME:    cond.field = ParamField::ToolName;    break;
      case ProtoField::RESOURCE_URI: cond.field = ParamField::ResourceUri; break;
      case ProtoField::PROMPT_NAME:  cond.field = ParamField::PromptName;  break;
      case ProtoField::ATTRIBUTE:
        cond.field = ParamField::Attribute;
        cond.attribute_key = pc.attribute_key();
        break;
      default:                       cond.field = ParamField::ToolName;    break;
      }

      switch (pc.matcher().match_pattern_case()) {
      case envoy::extensions::filters::http::ai_filters::mcp_auth::v3::StringMatcher::kExact:
        cond.matcher = {StringMatcher::Kind::Exact,  pc.matcher().exact()};  break;
      case envoy::extensions::filters::http::ai_filters::mcp_auth::v3::StringMatcher::kPrefix:
        cond.matcher = {StringMatcher::Kind::Prefix, pc.matcher().prefix()}; break;
      case envoy::extensions::filters::http::ai_filters::mcp_auth::v3::StringMatcher::kSuffix:
        cond.matcher = {StringMatcher::Kind::Suffix, pc.matcher().suffix()}; break;
      default:
        cond.matcher = {StringMatcher::Kind::Exact, ""};                     break;
      }

      policy.param_conditions.push_back(std::move(cond));
    }

    cfg->method_policies.push_back(std::move(policy));
  }

  // admin_method_prefix (deprecated) — only applied when method_policies is empty.
  if (cfg->method_policies.empty() && !proto.admin_method_prefix().empty()) {
    cfg->admin_method_prefix = proto.admin_method_prefix();
  }

  return cfg;
}

// ── createAiFilter ────────────────────────────────────────────────────────────
//
// Called once per stream (per JSON-RPC request) to instantiate the filter.
// `callbacks.addAiFilter(filter)` registers it with the AgenticChain being
// built — identical to FilterChainFactoryCallbacks::addStreamDecoderFilter().

void McpAuthFilterFactory::createAiFilter(
    AiProtocolManager::Chain::AiFilterChainCallbacks& callbacks,
    const Protobuf::Any& typed_config,
    Server::Configuration::FactoryContext& context) {
  auto config = parseConfig(typed_config, context);
  callbacks.addAiFilter(std::make_unique<McpAuthFilter>(std::move(config)));
}

// ── Static registration ───────────────────────────────────────────────────────
//
// Registers the factory under "envoy.ai_filters.mcp_auth" in the AiFilterFactory
// registry so AiProtocolManagerConfig::buildChain() can find it by name.

static Envoy::Registry::RegisterInternalFactory<McpAuthFilterFactory, AiProtocolManager::Chain::AiFilterFactory>
    mcp_auth_filter_factory_registered;

} // namespace McpAuth
} // namespace AiFilters
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
