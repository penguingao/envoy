#pragma once

#include <memory>
#include <string>
#include <vector>

#include "envoy/server/filter_config.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats_macros.h"

#include "source/extensions/filters/http/ai_protocol_manager/chain/agentic_chain.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter_factory.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain/inference_chain.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/request_decoder.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// clang-format off
#define AI_PROTOCOL_MANAGER_STATS(COUNTER)                                                         \
  COUNTER(rq_total)                                                                                \
  COUNTER(rq_inference)                                                                            \
  COUNTER(rq_agent)                                                                                \
  COUNTER(rq_classify_unknown)                                                                     \
  COUNTER(rq_decode_error)                                                                         \
  COUNTER(rq_encode_error)                                                                         \
  COUNTER(rq_payload_offloaded)                                                                    \
  COUNTER(rq_chain_stop)                                                                           \
  COUNTER(rq_local_reply)                                                                          \
  COUNTER(rq_dispatch_failure)
// clang-format on

struct AiProtocolManagerStats {
  AI_PROTOCOL_MANAGER_STATS(GENERATE_COUNTER_STRUCT)
};

enum class DispatchMode {
  Fallout,      // dispatch filter owns Http::AsyncClient (original mode)
  ChainForward, // dispatch filter calls continueDecoding(); Envoy router handles upstream
};

// Per-chain configuration built once from proto at startup.
// Holds the ordered AiFilterSpec list analogous to the HCM FilterChainFactory's
// filter config list.
struct ChainConfig {
  std::vector<Chain::AiFilterSpec> filters; // ordered; tail is always the *Dispatch filter
  DispatchMode dispatch_mode{DispatchMode::ChainForward};
};

// AiProtocolManagerConfig — created once per listener worker, shared across
// all streams on that worker via shared_ptr.
//
// Analogy to HCM:
//   HCM's FilterChainFactory  →  AiProtocolManagerConfig
//   createFilterChain(cbs)    →  createInferenceChain() / createAgenticChain()
//   addStreamDecoderFilter(f) →  AiFilterChainCallbacks::addAiFilter(f)
//
// At stream time the filter calls createInferenceChain() or createAgenticChain()
// once, analogous to FilterManager::createDownstreamFilterChain(). The returned
// chain object is owned by the stream for its lifetime.
class AiProtocolManagerConfig {
public:
  AiProtocolManagerConfig(ChainConfig inference_chain_config,
                          ChainConfig agentic_chain_config,
                          Codec::DecoderConfig decoder_config,
                          AiProtocolManagerStats stats,
                          Server::Configuration::FactoryContext& context);

  // Build a fresh InferenceChain for one stream.
  //
  // Iterates inference_chain_config_.filters in order. For each AiFilterSpec:
  //   1. Looks up AiFilterFactory in InferenceAiFilterFactoryRegistry (then
  //      falls back to AiFilterFactoryRegistry for cross-cutting filters).
  //   2. Calls factory->createAiFilter(chain, spec.typed_config, context_).
  //      The factory calls chain.addAiFilter(filter) internally.
  //   3. After all filters: calls chain.finalizeInterests().
  Chain::InferenceChainPtr createInferenceChain() const;

  // Build a fresh AgenticChain for one stream.  Same pattern as above using
  // AgenticAiFilterFactoryRegistry then AiFilterFactoryRegistry.
  Chain::AgenticChainPtr createAgenticChain() const;

  const Codec::DecoderConfig& decoderConfig() const { return decoder_config_; }
  AiProtocolManagerStats& stats() { return stats_; }
  Server::Configuration::FactoryContext& factoryContext() const { return context_; }

private:
  template <typename ChainT>
  std::unique_ptr<ChainT> buildChain(const ChainConfig& cfg) const;

  ChainConfig inference_chain_config_;
  ChainConfig agentic_chain_config_;
  Codec::DecoderConfig decoder_config_;
  AiProtocolManagerStats stats_;
  Server::Configuration::FactoryContext& context_;
};

using AiProtocolManagerConfigSharedPtr = std::shared_ptr<AiProtocolManagerConfig>;

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
