#pragma once

#include <string>

#include "envoy/extensions/filters/ai/auto_router/v3/auto_router.pb.h"
#include "envoy/extensions/filters/ai/auto_router/v3/auto_router.pb.validate.h"

#include "source/extensions/filters/ai/ai_filter.h"

namespace Envoy {
namespace Extensions {
namespace AiFilters {
namespace AutoRouter {

class AutoRouterFilterConfigFactory : public NamedAiFilterConfigFactory {
public:
  absl::StatusOr<AiFilterFactoryCb>
  createFilterFactoryFromProto(const Protobuf::Message& config, const std::string& stats_prefix,
                               Server::Configuration::ServerFactoryContext& context) override;

  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return std::make_unique<envoy::extensions::filters::ai::auto_router::v3::AutoRouter>();
  }

  std::string name() const override { return "envoy.filters.ai.auto_router"; }
};

DECLARE_FACTORY(AutoRouterFilterConfigFactory);

} // namespace AutoRouter
} // namespace AiFilters
} // namespace Extensions
} // namespace Envoy
