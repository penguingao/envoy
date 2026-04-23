#pragma once

#include <functional>
#include <memory>
#include <string>

#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"

#include "source/extensions/filters/http/ai_protocol_manager/chain/ai_filter.h"

#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Chain {

// AiFilterChainCallbacks — the callback interface the factory uses to populate
// an AiFilterChain. Mirrors Envoy's FilterChainFactoryCallbacks.
//
// The AiProtocolManagerConfig calls factory->createAiFilter(callbacks) for
// each configured filter; the factory constructs its AiFilter and hands it to
// the chain via addAiFilter(). This decouples the chain-building logic from
// the filter implementation.
class AiFilterChainCallbacks {
public:
  virtual ~AiFilterChainCallbacks() = default;

  // Add an instantiated AiFilter to the chain being built. Filters are added
  // in the order the config lists them; that order is the execution order.
  virtual void addAiFilter(AiFilterPtr filter) = 0;
};

// AiFilterFactory — the interface every pluggable AiFilter author implements.
//
// Registration:
//   REGISTER_FACTORY(MyFilterFactory, Chain::AiFilterFactory);
//
// Config:
//   The proto config for this filter is passed in as an Any. The factory is
//   responsible for unpacking it (MessageUtil::anyConvertAndValidate<MyProto>).
class AiFilterFactory {
public:
  virtual ~AiFilterFactory() = default;

  // Name used in the AiProtocolManager proto config's `filters[].name` field.
  virtual std::string name() const = 0;

  // Instantiate one AiFilter per stream and add it to the chain.
  // `typed_config` is the Any-packed per-filter configuration from the listener.
  // `context` provides access to cluster manager, dispatcher, stats, etc.
  virtual void createAiFilter(AiFilterChainCallbacks& callbacks,
                              const Protobuf::Any& typed_config,
                              Server::Configuration::FactoryContext& context) = 0;

  // Return a default-initialised (empty) proto message of the type that
  // `typed_config` will contain — used for config validation at startup.
  virtual ProtobufTypes::MessagePtr createEmptyConfigProto() = 0;
};

// Registry alias — same lookup pattern as NamedHttpFilterConfigFactory.
using AiFilterFactoryRegistry = Registry::FactoryRegistry<AiFilterFactory>;

// Per-chain configuration built from proto at startup. Holds the ordered list
// of {name, typed_config} pairs that createAiFilter() iterates over.
struct AiFilterSpec {
  std::string   name;
  Protobuf::Any typed_config;
};

} // namespace Chain
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
