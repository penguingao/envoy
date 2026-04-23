#include "source/extensions/filters/http/ai_protocol_manager/config.h"

#include "envoy/registry/registry.h"

#include "source/common/protobuf/protobuf.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter_factory.h"
#include "source/extensions/filters/http/ai_protocol_manager/filter.h"

#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// ── buildConfig ───────────────────────────────────────────────────────────────
//
// Parses AiProtocolManager proto → AiProtocolManagerConfig.
//
// proto.ai_filters[] is an ordered list of TypedExtensionConfig entries.  Each
// entry maps directly to a Chain::AiFilterSpec{name, typed_config} and is
// appended to the agentic chain's filter list in declaration order.

AiProtocolManagerConfigSharedPtr
AiProtocolManagerFilterConfigFactory::buildConfig(const ProtoConfig& proto,
                                                   const std::string& stats_prefix,
                                                   Server::Configuration::FactoryContext& context) {
  ChainConfig inference_cfg;
  ChainConfig agentic_cfg;

  for (const auto& entry : proto.ai_filters()) {
    Chain::AiFilterSpec spec;
    spec.name = entry.name();
    spec.typed_config = entry.typed_config();
    agentic_cfg.filters.push_back(std::move(spec));
  }

  // Default decoder config.
  Codec::DecoderConfig decoder_cfg;

  // Generate stats.
  const std::string p = absl::StrCat(stats_prefix, "ai_protocol_manager.");
  AiProtocolManagerStats stats{
      AI_PROTOCOL_MANAGER_STATS(POOL_COUNTER_PREFIX(context.scope(), p))};

  return std::make_shared<AiProtocolManagerConfig>(std::move(inference_cfg),
                                                   std::move(agentic_cfg),
                                                   std::move(decoder_cfg),
                                                   std::move(stats),
                                                   context);
}

// ── createFilterFactoryFromProtoTyped ─────────────────────────────────────────
//
// Called by FactoryBase::createFilterFactoryFromProto after downcasting the
// generic Protobuf::Message to the typed ProtoConfig.
//
//   1. Build the shared config object (once per listener worker).
//   2. Return a lambda that creates one AiProtocolManagerFilter per stream.

Http::FilterFactoryCb
AiProtocolManagerFilterConfigFactory::createFilterFactoryFromProtoTyped(
    const ProtoConfig& proto_config,
    const std::string& stats_prefix,
    Server::Configuration::FactoryContext& context) {

  auto config = buildConfig(proto_config, stats_prefix, context);

  return [config](Http::FilterChainFactoryCallbacks& callbacks) {
    auto filter = std::make_shared<AiProtocolManagerFilter>(config);
    // Add as both decoder and encoder so chain-forward response path works.
    callbacks.addStreamFilter(filter);
  };
}

// ── Static registration ───────────────────────────────────────────────────────

REGISTER_FACTORY(AiProtocolManagerFilterConfigFactory,
                 Server::Configuration::NamedHttpFilterConfigFactory);

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
