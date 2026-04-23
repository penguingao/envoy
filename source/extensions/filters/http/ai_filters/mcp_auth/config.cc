#include "source/extensions/filters/http/ai_filters/mcp_auth/config.h"

#include "envoy/registry/registry.h"

#include "source/common/protobuf/utility.h"
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
// Field defaults (applied when the proto field is unset or empty):
//   identity_header      → "x-mcp-identity"
//   admin_method_prefix  → "admin/"
//   "initialize" is always inserted into allowed_unauthenticated_methods
//   regardless of configuration.

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

  // admin_method_prefix — default "admin/" set by McpAuthFilterConfig ctor.
  if (!proto.admin_method_prefix().empty()) {
    cfg->admin_method_prefix = proto.admin_method_prefix();
  }

  // allowed_unauthenticated_methods — "initialize" always present (added by ctor).
  for (const auto& method : proto.allowed_unauthenticated_methods()) {
    cfg->allowed_unauthenticated_methods.insert(method);
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

REGISTER_FACTORY(McpAuthFilterFactory, AiProtocolManager::Chain::AiFilterFactory);

} // namespace McpAuth
} // namespace AiFilters
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
