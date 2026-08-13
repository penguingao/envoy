#include "source/extensions/filters/http/ai_protocol_manager/config.h"

#include "envoy/registry/registry.h"

#include "source/common/config/utility.h"
#include "source/extensions/filters/ai/ai_filter.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer_impl.h"
#include "source/extensions/filters/http/ai_protocol_manager/filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

absl::StatusOr<Http::FilterFactoryCb>
AiProtocolManagerFilterConfigFactory::createFilterFactoryFromProtoTyped(
    const envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager&
        proto_config,
    const std::string& stats_prefix, DualInfo,
    Server::Configuration::ServerFactoryContext& context) {
  // One factory is shared by every stream on the chain. The in-memory
  // implementation is stateless, so a single shared instance is safe.
  absl::StatusOr<AiFilterFactoryCbs> ai_filters =
      compileAiFilters(proto_config.ai_filters(), stats_prefix, context);
  RETURN_IF_NOT_OK_REF(ai_filters.status());

  auto buffer_factory = std::make_shared<InMemoryExternalBufferFactory>();
  auto config = std::make_shared<const FilterConfig>(proto_config, std::move(*ai_filters));
  return [buffer_factory, config](Http::FilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addStreamFilter(std::make_shared<AiProtocolManagerFilter>(*buffer_factory, config));
  };
}

absl::StatusOr<Router::RouteSpecificFilterConfigConstSharedPtr>
AiProtocolManagerFilterConfigFactory::createRouteSpecificFilterConfigTyped(
    const envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManagerPerRoute&
        proto_config,
    Server::Configuration::ServerFactoryContext& context, ProtobufMessage::ValidationVisitor&) {
  std::optional<AiFilterFactoryCbs> ai_filters;
  if (proto_config.has_ai_filters()) {
    absl::StatusOr<AiFilterFactoryCbs> compiled =
        compileAiFilters(proto_config.ai_filters(), "", context);
    RETURN_IF_NOT_OK_REF(compiled.status());
    ai_filters = std::move(*compiled);
  }
  return std::make_shared<const RouteConfig>(proto_config, std::move(ai_filters));
}

/**
 * Static registration for the AI Protocol Manager filter as a downstream and an
 * upstream HTTP filter. @see RegisterFactory.
 */
REGISTER_FACTORY(AiProtocolManagerFilterConfigFactory,
                 Server::Configuration::NamedHttpFilterConfigFactory);
REGISTER_FACTORY(UpstreamAiProtocolManagerFilterConfigFactory,
                 Server::Configuration::UpstreamHttpFilterConfigFactory);

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
