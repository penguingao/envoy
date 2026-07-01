#include "source/extensions/filters/http/ai_protocol_manager/config.h"

#include "envoy/registry/registry.h"

#include "source/common/config/utility.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer_config.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer_impl.h"
#include "source/extensions/filters/http/ai_protocol_manager/filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

Http::FilterFactoryCb AiProtocolManagerFilterConfigFactory::createFilterFactoryFromProtoTyped(
    const envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager&
        proto_config,
    const std::string&, Server::Configuration::FactoryContext& context) {
  // One factory is shared by every stream on the chain. Each backing-store
  // implementation is stateless (or holds only shared, thread-safe handles), so a
  // single shared instance is safe.
  ExternalBufferFactorySharedPtr buffer_factory;
  if (proto_config.has_external_buffer()) {
    // Resolve the configured store from the typed-extension registry and let it
    // build the factory.
    auto& config_factory = Config::Utility::getAndCheckFactory<ExternalBufferConfigFactory>(
        proto_config.external_buffer());
    ProtobufTypes::MessagePtr message = Config::Utility::translateToFactoryConfig(
        proto_config.external_buffer(), context.messageValidationVisitor(), config_factory);
    buffer_factory = config_factory.createExternalBufferFactory(*message, context);
  } else {
    // No store configured: default to the in-memory store, preserving the
    // filter's original behavior.
    buffer_factory = std::make_shared<InMemoryExternalBufferFactory>();
  }
  return [buffer_factory](Http::FilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addStreamFilter(std::make_shared<AiProtocolManagerFilter>(*buffer_factory));
  };
}

/**
 * Static registration for the AI Protocol Manager filter. @see RegisterFactory.
 */
REGISTER_FACTORY(AiProtocolManagerFilterConfigFactory,
                 Server::Configuration::NamedHttpFilterConfigFactory);

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
