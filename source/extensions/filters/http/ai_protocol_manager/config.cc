#include "source/extensions/filters/http/ai_protocol_manager/config.h"

#include <memory>

#include "envoy/registry/registry.h"

#include "source/extensions/filters/http/ai_protocol_manager/ai_protocol_manager.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

absl::StatusOr<Http::FilterFactoryCb>
AiProtocolManagerFilterFactory::createFilterFactoryFromProtoTyped(
    const envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager&,
    const std::string&, Server::Configuration::FactoryContext&) {
  return [](Http::FilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addStreamFilter(std::make_shared<AiProtocolManagerFilter>());
  };
}

REGISTER_FACTORY(AiProtocolManagerFilterFactory,
                 Server::Configuration::NamedHttpFilterConfigFactory);

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
