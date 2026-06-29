#pragma once

#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter_chain.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Chain {

// AgenticChain — typed façade over AiFilterChain for the agentic sub-chain.
//
// Symmetric to InferenceChain. Future helpers (sessionId(), agentTarget())
// would expose AgentPayload fields without forcing sub-chain filters to
// std::get<AgentPayload> themselves.
class AgenticChain : public AiFilterChain {
public:
  AgenticChain() = default;

  // Future: AgentPayload-specific accessors.
  // const AgentTarget& agentTarget() const;
  // const std::string& sessionId() const;
};

using AgenticChainPtr = std::unique_ptr<AgenticChain>;

// Separate factory registry for agentic-only AiFilters.
class AgenticAiFilterFactory : public AiFilterFactory {};
using AgenticAiFilterFactoryRegistry = Registry::FactoryRegistry<AgenticAiFilterFactory>;

} // namespace Chain
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
