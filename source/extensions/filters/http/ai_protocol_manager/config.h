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

  // ai_protocol_manager is NOT a terminal filter. A proxy typically handles
  // both AI and non-AI traffic on the same ingress; per-route config decides
  // whether this filter engages. Classified Inference requests that reach
  // the filter with a dispatch target configured still terminate themselves
  // (StopIteration + Http::AsyncClient + encodeHeaders), but every other
  // request path (Unknown classification, no inference_dispatch configured,
  // the not-yet-implemented agent path) returns Continue so router and the
  // normal routing machinery handle the request. Operators scope the filter
  // per-route via typed_per_filter_config (AiProtocolManagerOverride).
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
