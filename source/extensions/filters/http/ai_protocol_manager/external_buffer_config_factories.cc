#include <memory>
#include <string>
#include <utility>

#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.h"
#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.validate.h"
#include "envoy/registry/registry.h"

#include "source/extensions/common/async_files/async_file_manager_factory.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer_config.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer_impl.h"
#include "source/extensions/filters/http/ai_protocol_manager/filesystem_external_buffer.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// Selects the in-memory (heap) store. Stateless, so a single shared factory
// serves every stream.
class InMemoryExternalBufferConfigFactory
    : public ExternalBufferConfigFactoryBase<
          envoy::extensions::filters::http::ai_protocol_manager::v3::InMemoryBuffer> {
public:
  InMemoryExternalBufferConfigFactory()
      : ExternalBufferConfigFactoryBase(
            "envoy.http.ai_protocol_manager.external_buffers.in_memory") {}

private:
  ExternalBufferFactorySharedPtr createExternalBufferFactoryTyped(
      const envoy::extensions::filters::http::ai_protocol_manager::v3::InMemoryBuffer&,
      Server::Configuration::FactoryContext&) override {
    return std::make_shared<InMemoryExternalBufferFactory>();
  }
};

// Selects the anonymous-file store, resolving the shared thread-pool
// AsyncFileManager from the singleton manager.
class FileSystemExternalBufferConfigFactory
    : public ExternalBufferConfigFactoryBase<
          envoy::extensions::filters::http::ai_protocol_manager::v3::FileSystemBuffer> {
public:
  FileSystemExternalBufferConfigFactory()
      : ExternalBufferConfigFactoryBase(
            "envoy.http.ai_protocol_manager.external_buffers.file_system") {}

private:
  ExternalBufferFactorySharedPtr createExternalBufferFactoryTyped(
      const envoy::extensions::filters::http::ai_protocol_manager::v3::FileSystemBuffer& config,
      Server::Configuration::FactoryContext& context) override {
    auto& server_context = context.serverFactoryContext();
    // The factory owns the id->manager registry; keep it alive alongside the
    // manager (the singleton manager does not retain it).
    auto manager_factory =
        Common::AsyncFiles::AsyncFileManagerFactory::singleton(&server_context.singletonManager());
    auto manager = manager_factory->getAsyncFileManager(config.manager_config());
    return std::make_shared<FilesystemExternalBufferFactory>(
        std::move(manager_factory), std::move(manager), config.buffer_path(),
        config.memory_buffer_bytes());
  }
};

REGISTER_FACTORY(InMemoryExternalBufferConfigFactory, ExternalBufferConfigFactory);
REGISTER_FACTORY(FileSystemExternalBufferConfigFactory, ExternalBufferConfigFactory);

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
