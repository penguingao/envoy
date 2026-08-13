#include "source/extensions/filters/ai/auto_router/config.h"

#include <memory>

#include "envoy/registry/registry.h"

#include "source/common/protobuf/utility.h"
#include "source/extensions/filters/ai/auto_router/filter.h"

namespace Envoy {
namespace Extensions {
namespace AiFilters {
namespace AutoRouter {

absl::StatusOr<AiFilterFactoryCb> AutoRouterFilterConfigFactory::createFilterFactoryFromProto(
    const Protobuf::Message& config, const std::string&,
    Server::Configuration::ServerFactoryContext& context) {
  const auto& typed_config = MessageUtil::downcastAndValidate<const AutoRouterProto&>(
      config, context.messageValidationVisitor());

  // One shared, immutable config for every stream on this chain; the per-stream
  // filter holds nothing but a pointer to it.
  auto shared = std::make_shared<const Config>(typed_config, context);
  return [shared](AiFilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addFilter(std::make_unique<Filter>(shared));
  };
}

/**
 * Static registration for the AI auto router. @see NamedAiFilterConfigFactory.
 */
REGISTER_FACTORY(AutoRouterFilterConfigFactory, NamedAiFilterConfigFactory);

} // namespace AutoRouter
} // namespace AiFilters
} // namespace Extensions
} // namespace Envoy
