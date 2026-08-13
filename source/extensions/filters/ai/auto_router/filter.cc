#include "source/extensions/filters/ai/auto_router/filter.h"

#include <algorithm>
#include <utility>

#include "source/common/coroutine/status_macros.h"

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"

namespace Envoy {
namespace Extensions {
namespace AiFilters {
namespace AutoRouter {

namespace {

constexpr uint32_t DefaultMaxScanBytes = 64 * 1024;
constexpr absl::string_view DefaultHeaderName = "x-envoy-ai-route";

// Appends whatever prompt text `content` holds inline, up to `budget`.
// Offloaded parts contribute their size but not their text: reading them back
// means going to the buffer, which this filter does not do yet.
void collectContent(const nlohmann::json& content, uint32_t budget, RequestSignals& signals) {
  if (HttpFilters::AiProtocolManager::JsonWithExtBuf::isExternalRef(content)) {
    const auto ref = HttpFilters::AiProtocolManager::JsonWithExtBuf::externalRef(content);
    if (ref.ok()) {
      signals.prompt_bytes += ref->length;
    }
    return;
  }
  if (content.is_string()) {
    const std::string& text = content.get_ref<const std::string&>();
    signals.prompt_bytes += text.size();
    if (signals.inline_prompt.size() < budget) {
      const size_t room = budget - signals.inline_prompt.size();
      signals.inline_prompt.append(text, 0, std::min(room, text.size()));
      signals.inline_prompt.push_back('\n');
    }
    return;
  }
  if (content.is_array()) {
    for (const nlohmann::json& part : content) {
      if (!part.is_object()) {
        continue;
      }
      const auto type = part.find("type");
      if (type != part.end() && type->is_string() &&
          type->get_ref<const std::string&>() == "image_url") {
        signals.has_images = true;
      }
      if (const auto text = part.find("text"); text != part.end()) {
        collectContent(*text, budget, signals);
      }
      if (const auto image = part.find("image_url"); image != part.end() && image->is_object()) {
        signals.has_images = true;
        if (const auto url = image->find("url"); url != image->end()) {
          // Counted for size, never matched against: a data URI is not prose.
          if (HttpFilters::AiProtocolManager::JsonWithExtBuf::isExternalRef(*url)) {
            const auto ref = HttpFilters::AiProtocolManager::JsonWithExtBuf::externalRef(*url);
            if (ref.ok()) {
              signals.prompt_bytes += ref->length;
            }
          }
        }
      }
    }
  }
}

} // namespace

CompiledRoute::CompiledRoute(const AutoRouterProto::Route& proto,
                             Server::Configuration::ServerFactoryContext& context)
    : name_(proto.name()), weight_(proto.weight() == 0 ? 1 : proto.weight()) {
  keywords_.reserve(proto.keywords().size());
  for (const std::string& keyword : proto.keywords()) {
    keywords_.push_back(absl::AsciiStrToLower(keyword));
  }
  for (const auto& regex : proto.regexes()) {
    envoy::type::matcher::v3::StringMatcher matcher;
    *matcher.mutable_safe_regex() = regex;
    regexes_.push_back(std::make_unique<Matchers::StringMatcherImpl>(matcher, context));
  }
  if (proto.has_structural()) {
    has_structural_ = true;
    const auto& structural = proto.structural();
    if (structural.has_has_tools()) {
      want_tools_ = structural.has_tools().value();
    }
    if (structural.has_has_images()) {
      want_images_ = structural.has_images().value();
    }
    min_prompt_bytes_ = structural.min_prompt_bytes();
    max_prompt_bytes_ = structural.max_prompt_bytes();
  }
}

std::optional<uint32_t> CompiledRoute::score(const RequestSignals& signals) const {
  if (has_structural_) {
    if (want_tools_.has_value() && *want_tools_ != signals.has_tools) {
      return std::nullopt;
    }
    if (want_images_.has_value() && *want_images_ != signals.has_images) {
      return std::nullopt;
    }
    if (signals.prompt_bytes < min_prompt_bytes_) {
      return std::nullopt;
    }
    if (max_prompt_bytes_ != 0 && signals.prompt_bytes > max_prompt_bytes_) {
      return std::nullopt;
    }
  }

  const std::string haystack = absl::AsciiStrToLower(signals.inline_prompt);
  uint32_t hits = 0;
  for (const std::string& keyword : keywords_) {
    if (!keyword.empty() && absl::StrContains(haystack, keyword)) {
      ++hits;
    }
  }
  for (const Matchers::StringMatcherPtr& regex : regexes_) {
    if (regex->match(signals.inline_prompt)) {
      ++hits;
    }
  }

  // Nothing matched. A route that named structural preconditions still counts as
  // a match -- those held, and they are evidence in themselves -- but one that
  // only named keywords has no claim on this request, or every route would match
  // everything and default_route would be unreachable.
  if (hits == 0 && !has_structural_) {
    return std::nullopt;
  }
  return hits * weight_;
}

Config::Config(const AutoRouterProto& proto, Server::Configuration::ServerFactoryContext& context)
    : default_route_(proto.default_route()), verdict_(proto.verdict()),
      header_name_(proto.header_name().empty() ? std::string(DefaultHeaderName)
                                               : proto.header_name()),
      max_scan_bytes_(proto.max_scan_bytes() == 0 ? DefaultMaxScanBytes : proto.max_scan_bytes()) {
  routes_.reserve(proto.routes().size());
  for (const auto& route : proto.routes()) {
    routes_.emplace_back(route, context);
  }
}

absl::string_view Config::pick(const RequestSignals& signals) const {
  const CompiledRoute* best = nullptr;
  uint32_t best_score = 0;
  for (const CompiledRoute& route : routes_) {
    const std::optional<uint32_t> score = route.score(signals);
    if (!score.has_value()) {
      continue;
    }
    // Strictly greater, so declaration order breaks a tie.
    if (best == nullptr || *score > best_score) {
      best = &route;
      best_score = *score;
    }
  }
  // A route that matched structurally but scored nothing still wins over the
  // default: its preconditions are evidence in themselves.
  return best != nullptr ? absl::string_view(best->name()) : absl::string_view(default_route_);
}

RequestSignals Filter::collectSignals(const InferenceRequest& request, uint32_t max_scan_bytes) {
  RequestSignals signals;
  const nlohmann::json* tools = request.tools();
  signals.has_tools = tools != nullptr && !tools->empty();

  if (const nlohmann::json* messages = request.messages(); messages != nullptr) {
    for (const nlohmann::json& message : *messages) {
      if (!message.is_object()) {
        continue;
      }
      if (const auto content = message.find("content"); content != message.end()) {
        collectContent(*content, max_scan_bytes, signals);
      }
    }
  }
  return signals;
}

Coroutine::Task<absl::StatusOr<PostDecodeAction>>
Filter::decode(InferenceRequestGetter& getter, InferenceRequestForwarder& forwarder) {
  ASSIGN_OR_CO_RETURN(InferenceRequestPtr request, co_await getter.get());

  const RequestSignals signals = collectSignals(*request, config_->maxScanBytes());
  const absl::string_view target = config_->pick(signals);

  if (!target.empty()) {
    ENVOY_LOG(debug, "auto_router: routing to '{}' ({} prompt bytes, tools={}, images={})", target,
              signals.prompt_bytes, signals.has_tools, signals.has_images);
    const AutoRouterProto::Verdict verdict = config_->verdict();
    if (verdict == AutoRouterProto::SET_HEADER || verdict == AutoRouterProto::BOTH) {
      if (callbacks_ != nullptr) {
        if (Http::RequestHeaderMapOptRef headers = callbacks_->requestHeaders();
            headers.has_value()) {
          headers->setReferenceKey(config_->headerName(), std::string(target));
        }
      }
    }
    if (verdict == AutoRouterProto::REWRITE_MODEL || verdict == AutoRouterProto::BOTH) {
      // Modifies the payload, so it is re-serialized rather than replayed.
      request->setModel(target);
    }
  }

  CO_RETURN_IF_ERROR(co_await forwarder.forward(std::move(request)));
  co_return PostDecodeAction::Skip;
}

} // namespace AutoRouter
} // namespace AiFilters
} // namespace Extensions
} // namespace Envoy
