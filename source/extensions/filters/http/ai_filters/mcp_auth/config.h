#pragma once

#include <memory>
#include <string>

#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"

#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter_factory.h"

#include "envoy/extensions/filters/http/ai_filters/mcp_auth/v3/mcp_auth.pb.h"
#include "envoy/extensions/filters/http/ai_filters/mcp_auth/v3/mcp_auth.pb.validate.h"

#include "absl/container/flat_hash_set.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiFilters {
namespace McpAuth {

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

  // Methods whose name starts with this prefix are restricted to the "admin"
  // principal. Default: "admin/".
  std::string admin_method_prefix{"admin/"};

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
