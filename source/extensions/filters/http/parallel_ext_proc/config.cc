#include "source/extensions/filters/http/parallel_ext_proc/config.h"

#include "source/extensions/filters/http/ext_proc/config.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ParallelExtProc {

Http::FilterFactoryCb ParallelExtProcFilterFactory::createFilterFactoryFromProtoTyped(
    const envoy::extensions::filters::http::parallel_ext_proc::v3::ParallelExternalProcessor&
        proto_config,
    const std::string& stats_prefix, Server::Configuration::FactoryContext& context) {

  // Build per-processor filter factory callbacks by reusing the ext_proc factory.
  ExternalProcessing::ExternalProcessingFilterConfig ext_proc_factory;

  std::vector<ProcessorInfo> processors;
  processors.reserve(proto_config.processors_size());

  for (const auto& proc_instance : proto_config.processors()) {
    // Use the public createFilterFactoryFromProto interface.
    // The ext_proc filter will run in our custom filter chain with ChainCallbacks
    // providing upstreamCallbacks(), so ext_proc will behave correctly.
    auto factory_cb_or_error = ext_proc_factory.createFilterFactoryFromProto(
        proc_instance.ext_proc_config(), stats_prefix, context);

    THROW_IF_NOT_OK_REF(factory_cb_or_error.status());

    processors.push_back(ProcessorInfo{
        proc_instance.name(),
        proc_instance.priority(),
        std::move(factory_cb_or_error.value()),
        proc_instance.can_modify_body(),
    });
  }

  const std::chrono::milliseconds aggregate_timeout =
      std::chrono::milliseconds(PROTOBUF_GET_MS_OR_DEFAULT(proto_config, aggregate_timeout, 0));

  auto filter_config = std::make_shared<ParallelFilterConfig>(
      std::move(processors), proto_config.failure_policy(), aggregate_timeout);

  return [filter_config](Http::FilterChainFactoryCallbacks& callbacks) {
    callbacks.addStreamFilter(std::make_shared<ParallelExtProcFilter>(filter_config));
  };
}

REGISTER_FACTORY(ParallelExtProcFilterFactory,
                 Server::Configuration::NamedHttpFilterConfigFactory);

} // namespace ParallelExtProc
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
