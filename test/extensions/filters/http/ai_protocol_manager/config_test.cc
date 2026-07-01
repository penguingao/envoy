#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.h"
#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.validate.h"

#include "source/extensions/filters/http/ai_protocol_manager/config.h"

#include "test/mocks/server/factory_context.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::NiceMock;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

// The factory builds a stream filter (the filter wires both decode and encode
// paths, so it registers as a stream filter rather than a decoder-only filter).
TEST(AiProtocolManagerConfigTest, CreatesStreamFilterFromProto) {
  envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager proto_config;
  NiceMock<Server::Configuration::MockFactoryContext> context;

  AiProtocolManagerFilterConfigFactory factory;
  Http::FilterFactoryCb cb =
      factory.createFilterFactoryFromProto(proto_config, "stats", context).value();

  Http::MockFilterChainFactoryCallbacks filter_callbacks;
  EXPECT_CALL(filter_callbacks, addStreamFilter(_));
  cb(filter_callbacks);
}

// The empty (default) config proto produced by the factory yields a working
// filter factory too.
TEST(AiProtocolManagerConfigTest, CreatesStreamFilterFromEmptyProto) {
  AiProtocolManagerFilterConfigFactory factory;
  auto empty_proto = factory.createEmptyConfigProto();
  ASSERT_NE(empty_proto, nullptr);
  const auto& proto_config = *Envoy::Protobuf::DynamicCastMessage<
      envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager>(
      empty_proto.get());

  NiceMock<Server::Configuration::MockFactoryContext> context;
  Http::FilterFactoryCb cb =
      factory.createFilterFactoryFromProto(proto_config, "stats", context).value();

  Http::MockFilterChainFactoryCallbacks filter_callbacks;
  EXPECT_CALL(filter_callbacks, addStreamFilter(_));
  cb(filter_callbacks);
}

// Referencing the in-memory store via external_buffer resolves the typed
// extension and builds a working filter factory.
TEST(AiProtocolManagerConfigTest, SelectsInMemoryBufferBackend) {
  envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager proto_config;
  auto& ext = *proto_config.mutable_external_buffer();
  ext.set_name("in_memory");
  ASSERT_TRUE(ext.mutable_typed_config()->PackFrom(
      envoy::extensions::filters::http::ai_protocol_manager::v3::InMemoryBuffer()));
  NiceMock<Server::Configuration::MockFactoryContext> context;

  AiProtocolManagerFilterConfigFactory factory;
  Http::FilterFactoryCb cb =
      factory.createFilterFactoryFromProto(proto_config, "stats", context).value();

  Http::MockFilterChainFactoryCallbacks filter_callbacks;
  EXPECT_CALL(filter_callbacks, addStreamFilter(_));
  cb(filter_callbacks);
}

// Referencing the filesystem store resolves the typed extension, builds the
// async file manager from the singleton manager, and yields a working factory.
TEST(AiProtocolManagerConfigTest, SelectsFileSystemBufferBackend) {
  envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager proto_config;
  envoy::extensions::filters::http::ai_protocol_manager::v3::FileSystemBuffer fs;
  fs.set_buffer_path("/tmp");
  fs.mutable_manager_config()->mutable_thread_pool()->set_thread_count(1);
  auto& ext = *proto_config.mutable_external_buffer();
  ext.set_name("file_system");
  ASSERT_TRUE(ext.mutable_typed_config()->PackFrom(fs));
  NiceMock<Server::Configuration::MockFactoryContext> context;

  AiProtocolManagerFilterConfigFactory factory;
  Http::FilterFactoryCb cb =
      factory.createFilterFactoryFromProto(proto_config, "stats", context).value();

  Http::MockFilterChainFactoryCallbacks filter_callbacks;
  EXPECT_CALL(filter_callbacks, addStreamFilter(_));
  cb(filter_callbacks);
}

// An external_buffer referencing an unregistered type is rejected.
TEST(AiProtocolManagerConfigTest, RejectsUnknownExternalBuffer) {
  envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager proto_config;
  auto& ext = *proto_config.mutable_external_buffer();
  ext.set_name("bogus");
  // Pack an arbitrary message whose type is not a registered external buffer.
  ASSERT_TRUE(ext.mutable_typed_config()->PackFrom(
      envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManager()));
  NiceMock<Server::Configuration::MockFactoryContext> context;

  AiProtocolManagerFilterConfigFactory factory;
  EXPECT_THROW(factory.createFilterFactoryFromProto(proto_config, "stats", context).value(),
               EnvoyException);
}

// The factory is registered under its well-known name and resolvable from the
// HTTP filter factory registry.
TEST(AiProtocolManagerConfigTest, IsRegistered) {
  auto* factory =
      Registry::FactoryRegistry<Server::Configuration::NamedHttpFilterConfigFactory>::getFactory(
          "envoy.filters.http.ai_protocol_manager");
  ASSERT_NE(factory, nullptr);
  EXPECT_EQ(factory->name(), "envoy.filters.http.ai_protocol_manager");
}

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
