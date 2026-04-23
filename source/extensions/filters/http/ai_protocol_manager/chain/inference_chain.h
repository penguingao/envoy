#pragma once

#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter_chain.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Chain {

// InferenceChain — typed façade over AiFilterChain for the inference sub-chain.
//
// Exists as a distinct type so that:
//   - InferenceFilterFactoryRegistry is searched independently from the agentic
//     registry, preventing cross-chain filter mis-registration.
//   - Future protocol-specific helpers (modelTarget(), samplingParams() accessors
//     that read InferencePayload fields) have a natural home without polluting
//     the shared AiFilterChain base.
//
// In v0 this is a pure pass-through to AiFilterChain; all run*() calls delegate
// directly. The typed distinction is enough to allow the config and filter code
// to treat the two chains as separate objects.
class InferenceChain : public AiFilterChain {
public:
  InferenceChain() = default;

  // Future: InferencePayload-specific accessors.
  // const ModelTarget& modelTarget() const;
  // const SamplingParams& samplingParams() const;
};

using InferenceChainPtr = std::unique_ptr<InferenceChain>;

// Separate factory registry for inference-only AiFilters.
// AiFilters that are only valid on the inference chain register here.
// Cross-cutting filters (rate-limit, PII scrub, logging) typically register
// in both AiFilterFactoryRegistry (agentic) and InferenceAiFilterFactoryRegistry.
class InferenceAiFilterFactory : public AiFilterFactory {};
using InferenceAiFilterFactoryRegistry = Registry::FactoryRegistry<InferenceAiFilterFactory>;

} // namespace Chain
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
