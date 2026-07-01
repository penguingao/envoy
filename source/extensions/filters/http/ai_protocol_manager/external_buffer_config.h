#pragma once

#include <memory>
#include <string>

#include "envoy/config/typed_config.h"
#include "envoy/server/factory_context.h"

#include "source/common/protobuf/protobuf.h"
#include "source/common/protobuf/utility.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// Config-time factory for an ExternalBuffer backing store, registered as a typed
// extension so the store the filter offloads into is pluggable. The filter's
// `external_buffer` field (a config.core.v3.TypedExtensionConfig) selects one of
// these by type; config.cc resolves it from the registry and calls
// createExternalBufferFactory() to build the per-chain ExternalBufferFactory.
class ExternalBufferConfigFactory : public Config::TypedFactory {
public:
  // Builds the ExternalBufferFactory shared by every stream on the chain. `config`
  // is the unpacked typed_config of the selected store.
  virtual ExternalBufferFactorySharedPtr
  createExternalBufferFactory(const Protobuf::Message& config,
                              Server::Configuration::FactoryContext& context) PURE;

  std::string category() const override { return "envoy.http.ai_protocol_manager.external_buffer"; }
};

// Convenience base that casts and validates the opaque config into the
// implementation's proto and dispatches to a typed hook. Mirrors
// Compression::Common::Compressor::CompressorLibraryFactoryBase.
template <class ConfigProto>
class ExternalBufferConfigFactoryBase : public ExternalBufferConfigFactory {
public:
  ExternalBufferFactorySharedPtr
  createExternalBufferFactory(const Protobuf::Message& proto_config,
                              Server::Configuration::FactoryContext& context) override {
    return createExternalBufferFactoryTyped(MessageUtil::downcastAndValidate<const ConfigProto&>(
                                                proto_config, context.messageValidationVisitor()),
                                            context);
  }

  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return std::make_unique<ConfigProto>();
  }

  std::string name() const override { return name_; }

protected:
  explicit ExternalBufferConfigFactoryBase(const std::string& name) : name_(name) {}

private:
  virtual ExternalBufferFactorySharedPtr
  createExternalBufferFactoryTyped(const ConfigProto& config,
                                   Server::Configuration::FactoryContext& context) PURE;

  const std::string name_;
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
