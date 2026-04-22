#pragma once

#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.h"
#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.validate.h"

#include "source/extensions/filters/http/ai_protocol_manager/filter.h"
#include "source/extensions/filters/http/common/factory_base.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

class AiProtocolManagerFilterConfigFactory
    : public Common::FactoryBase<
          envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager,
          envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManagerOverride> {
public:
  AiProtocolManagerFilterConfigFactory() : FactoryBase("envoy.filters.http.ai_protocol_manager") {}

private:
  Http::FilterFactoryCb createFilterFactoryFromProtoTyped(
      const envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager&
          proto_config,
      const std::string& stats_prefix, Server::Configuration::FactoryContext& context) override;

  Http::FilterFactoryCb createFilterFactoryFromProtoWithServerContextTyped(
      const envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager&
          proto_config,
      const std::string& stats_prefix,
      Server::Configuration::ServerFactoryContext& context) override;

  // ai_protocol_manager is terminal: it dispatches to the upstream itself via
  // Http::AsyncClient and forwards the response via encodeHeaders/encodeData.
  // The HCM does not need (and rejects) router after this filter. Requests
  // that the classifier returns Unknown for are handled in-filter with a
  // local 404 reply rather than passed through. Same pattern as mcp_router.
  bool isTerminalFilterByProtoTyped(
      const envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager&,
      Server::Configuration::ServerFactoryContext&) override {
    return true;
  }
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
