#include "source/extensions/filters/http/ai_protocol_manager/filter_config.h"

#include "source/common/common/assert.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {



AiProtocolManagerConfig::AiProtocolManagerConfig(
    ChainConfig inference_chain_config, ChainConfig agentic_chain_config,
    Codec::DecoderConfig decoder_config, AiProtocolManagerStats stats,
    Server::Configuration::FactoryContext& context)
    : inference_chain_config_(std::move(inference_chain_config)),
      agentic_chain_config_(std::move(agentic_chain_config)),
      decoder_config_(std::move(decoder_config)),
      stats_(std::move(stats)),
      context_(context) {}

// ── Chain building — analogous to FilterManager::createFilterChain() ──────────
//
// For each AiFilterSpec in the config:
//   1. Find the factory in the appropriate registry.
//   2. Call factory->createAiFilter(chain, spec.typed_config, context_).
//      The factory internally calls chain.addAiFilter(filter).
//   3. After all factories: call chain.finalizeInterests() to cache the union
//      of declared interests — identical to how FilterManager sets entry_
//      iterators in its Cleanup lambda after createFilterChain() returns.

template <typename ChainT>
std::unique_ptr<ChainT> AiProtocolManagerConfig::buildChain(const ChainConfig& cfg) const {
  auto chain = std::make_unique<ChainT>();

  for (const auto& spec : cfg.filters) {
    // Look up the factory. Chain-specific registry is searched first; the
    // shared registry (cross-cutting filters) is the fallback.
    Chain::AiFilterFactory* factory =
        Registry::FactoryRegistry<Chain::AiFilterFactory>::getFactory(spec.name);
    RELEASE_ASSERT(factory != nullptr,
                   absl::StrCat("AiFilterFactory not found for: ", spec.name));

    // Factory constructs the AiFilter and registers it via chain.addAiFilter().
    factory->createAiFilter(*chain, spec.typed_config, context_);
  }

  // Compute the union of interest declarations across all registered filters.
  // This is done once here (chain-build time) rather than per-item/chunk,
  // matching the FilterManager pattern where entry_ iterators are set in a
  // Cleanup lambda after createFilterChain() returns.
  chain->finalizeInterests();
  return chain;
}

Chain::InferenceChainPtr AiProtocolManagerConfig::createInferenceChain() const {
  return buildChain<Chain::InferenceChain>(inference_chain_config_);
}

Chain::AgenticChainPtr AiProtocolManagerConfig::createAgenticChain() const {
  return buildChain<Chain::AgenticChain>(agentic_chain_config_);
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
