#include "source/extensions/filters/http/ai_protocol_manager/inference_request.h"

#include <string>
#include <utility>

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

namespace {

// Walks `node` depth-first, appending every offload reference it reaches to
// `out`. Document order, so the result lines up with the order the payload was
// parsed in.
void collectOffloadedRanges(const nlohmann::json& node,
                            std::vector<JsonWithExtBuf::ExternalRef>& out) {
  if (JsonWithExtBuf::isExternalRef(node)) {
    const absl::StatusOr<JsonWithExtBuf::ExternalRef> ref = JsonWithExtBuf::externalRef(node);
    if (ref.ok()) {
      out.push_back(*ref);
    }
    return;
  }
  if (node.is_object() || node.is_array()) {
    for (const auto& child : node) {
      collectOffloadedRanges(child, out);
    }
  }
}

} // namespace

const nlohmann::json* InferenceRequest::member(absl::string_view name) const {
  const nlohmann::json& root = payload_.json();
  if (!root.is_object()) {
    return nullptr;
  }
  const auto it = root.find(std::string(name));
  if (it == root.end()) {
    return nullptr;
  }
  return &it.value();
}

std::optional<absl::string_view> InferenceRequest::model() const {
  const nlohmann::json* node = member("model");
  if (node == nullptr || !node->is_string()) {
    return std::nullopt;
  }
  return absl::string_view(node->get_ref<const std::string&>());
}

std::optional<bool> InferenceRequest::stream() const {
  const nlohmann::json* node = member("stream");
  if (node == nullptr || !node->is_boolean()) {
    return std::nullopt;
  }
  return node->get<bool>();
}

std::optional<std::int64_t> InferenceRequest::maxTokens() const {
  const nlohmann::json* node = member("max_tokens");
  if (node == nullptr || !node->is_number_integer()) {
    return std::nullopt;
  }
  return node->get<std::int64_t>();
}

const nlohmann::json* InferenceRequest::messages() const {
  const nlohmann::json* node = member("messages");
  return (node != nullptr && node->is_array()) ? node : nullptr;
}

const nlohmann::json* InferenceRequest::tools() const {
  const nlohmann::json* node = member("tools");
  return (node != nullptr && node->is_array()) ? node : nullptr;
}

void InferenceRequest::setModel(absl::string_view model) {
  nlohmann::json& root = mutableJson();
  if (!root.is_object()) {
    return;
  }
  root["model"] = std::string(model);
}

std::vector<JsonWithExtBuf::ExternalRef> InferenceRequest::offloadedRanges() const {
  std::vector<JsonWithExtBuf::ExternalRef> ranges;
  collectOffloadedRanges(payload_.json(), ranges);
  return ranges;
}

std::uint64_t InferenceRequest::offloadedBytes() const {
  std::uint64_t total = 0;
  for (const JsonWithExtBuf::ExternalRef& ref : offloadedRanges()) {
    total += ref.length;
  }
  return total;
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
