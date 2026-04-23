#pragma once

#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"

#include "source/extensions/filters/http/common/factory_base.h"
#include "source/extensions/filters/http/ai_protocol_manager/filter_config.h"

#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.h"
#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.validate.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

using ProtoConfig =
    envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager;

// AiProtocolManagerFilterConfigFactory — registers the filter under the name
// "envoy.filters.http.ai_protocol_manager" so it can be referenced from
// listener/route configuration.
//
// Pattern mirrors McpRouterFilterConfigFactory (mcp_router/config.h):
//   FactoryBase<ProtoConfig>::createFilterFactoryFromProtoTyped()
//     → build AiProtocolManagerConfigImpl from proto
//     → return lambda that creates AiProtocolManagerFilter per stream
class AiProtocolManagerFilterConfigFactory
    : public Common::FactoryBase<ProtoConfig> {
public:
  AiProtocolManagerFilterConfigFactory()
      : FactoryBase("envoy.filters.http.ai_protocol_manager") {}

private:
  Http::FilterFactoryCb createFilterFactoryFromProtoTyped(
      const ProtoConfig& proto_config, const std::string& stats_prefix,
      Server::Configuration::FactoryContext& context) override;

  // Build AiProtocolManagerConfig from the decoded proto fields.
  AiProtocolManagerConfigSharedPtr
  buildConfig(const ProtoConfig& proto, const std::string& stats_prefix,
              Server::Configuration::FactoryContext& context);
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
