#include "source/extensions/filters/http/ai_protocol_manager/ai_request.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "source/extensions/filters/http/ai_protocol_manager/field_streaming_session.h"
#include "source/extensions/filters/http/ai_protocol_manager/filter_manager.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

AiRequest::AiRequest(JsonWithExtBuf doc, FilterManager& manager, size_t filter_index)
    : doc_(std::move(doc)), manager_(manager), filter_index_(filter_index) {}

void AiRequest::registerFieldForStreaming(absl::string_view path) {
  std::string p(path);
  if (std::find(registered_paths_.begin(), registered_paths_.end(), p) == registered_paths_.end()) {
    registered_paths_.push_back(std::move(p));
  }
}

std::unique_ptr<FieldStreamingSession> AiRequest::stream() {
  std::vector<std::string> paths = std::move(registered_paths_);
  registered_paths_.clear();
  if (paths.empty()) {
    return nullptr;
  }
  return manager_.startStageStreaming(filter_index_, doc_, std::move(paths));
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
