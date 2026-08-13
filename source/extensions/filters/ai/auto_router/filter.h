#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "envoy/extensions/filters/ai/auto_router/v3/auto_router.pb.h"

#include "source/common/common/logger.h"
#include "source/common/common/matchers.h"
#include "source/extensions/filters/ai/ai_filter.h"

namespace Envoy {
namespace Extensions {
namespace AiFilters {
namespace AutoRouter {

using AutoRouterProto = envoy::extensions::filters::ai::auto_router::v3::AutoRouter;

// Signals a request offers, gathered once and scored against every route.
struct RequestSignals {
  // Prompt text available inline. Offloaded content is not here -- it is
  // counted in prompt_bytes but not read.
  std::string inline_prompt;
  uint64_t prompt_bytes{0};
  bool has_tools{false};
  bool has_images{false};
};

// One configured route, compiled.
class CompiledRoute {
public:
  CompiledRoute(const AutoRouterProto::Route& proto, Server::Configuration::ServerFactoryContext&);

  // Score for `signals`, or nullopt when a structural precondition rules this
  // route out. A zero score is a match with no positive evidence, which still
  // beats no match at all.
  std::optional<uint32_t> score(const RequestSignals& signals) const;

  const std::string& name() const { return name_; }

private:
  std::string name_;
  // Lower-cased, so matching is case-insensitive without allocating per probe.
  std::vector<std::string> keywords_;
  // Full matches over the scanned prompt, per the usual StringMatcher contract;
  // keywords are what cover the substring case.
  std::vector<Matchers::StringMatcherPtr> regexes_;
  uint32_t weight_{1};

  bool has_structural_{false};
  std::optional<bool> want_tools_;
  std::optional<bool> want_images_;
  uint32_t min_prompt_bytes_{0};
  uint32_t max_prompt_bytes_{0};
};

// Shared, immutable configuration.
class Config : public Logger::Loggable<Logger::Id::filter> {
public:
  Config(const AutoRouterProto& proto, Server::Configuration::ServerFactoryContext& context);

  // The winning route's name, or empty when nothing matched and no default is
  // configured.
  absl::string_view pick(const RequestSignals& signals) const;

  AutoRouterProto::Verdict verdict() const { return verdict_; }
  const Http::LowerCaseString& headerName() const { return header_name_; }
  uint32_t maxScanBytes() const { return max_scan_bytes_; }

private:
  std::vector<CompiledRoute> routes_;
  std::string default_route_;
  AutoRouterProto::Verdict verdict_;
  Http::LowerCaseString header_name_;
  uint32_t max_scan_bytes_;
};
using ConfigSharedPtr = std::shared_ptr<const Config>;

// Classifies a request and reports the target it picked.
//
// Reads only what the payload already holds: the prompt text that stayed
// inline, and the size of what did not. Reading offloaded content back is a
// later change; until then a request whose prompt was offloaded is classified
// on its structure and size alone, which is exactly the information that is
// free.
class Filter : public AiFilter, public Logger::Loggable<Logger::Id::filter> {
public:
  explicit Filter(ConfigSharedPtr config) : config_(std::move(config)) {}

  // AiFilter
  void setCallbacks(AiFilterCallbacks& callbacks) override { callbacks_ = &callbacks; }
  Coroutine::Task<absl::StatusOr<PostDecodeAction>>
  decode(InferenceRequestGetter& getter, InferenceRequestForwarder& forwarder) override;
  void onDestroy() override {}

  // Gathers what the request offers. Exposed for testing.
  static RequestSignals collectSignals(const InferenceRequest& request, uint32_t max_scan_bytes);

private:
  ConfigSharedPtr config_;
  AiFilterCallbacks* callbacks_{nullptr};
};

} // namespace AutoRouter
} // namespace AiFilters
} // namespace Extensions
} // namespace Envoy
