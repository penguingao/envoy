#pragma once

#include "envoy/extensions/filters/http/parallel_ext_proc/v3/parallel_ext_proc.pb.h"
#include "envoy/extensions/filters/http/parallel_ext_proc/v3/parallel_ext_proc.pb.validate.h"

#include "source/extensions/filters/http/common/factory_base.h"
#include "source/extensions/filters/http/parallel_ext_proc/parallel_filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ParallelExtProc {

class ParallelExtProcFilterFactory
    : public Common::FactoryBase<
          envoy::extensions::filters::http::parallel_ext_proc::v3::ParallelExternalProcessor> {
public:
  ParallelExtProcFilterFactory() : FactoryBase("envoy.filters.http.parallel_ext_proc") {}

private:
  Http::FilterFactoryCb createFilterFactoryFromProtoTyped(
      const envoy::extensions::filters::http::parallel_ext_proc::v3::ParallelExternalProcessor&
          proto_config,
      const std::string& stats_prefix,
      Server::Configuration::FactoryContext& context) override;
};

DECLARE_FACTORY(ParallelExtProcFilterFactory);

} // namespace ParallelExtProc
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
