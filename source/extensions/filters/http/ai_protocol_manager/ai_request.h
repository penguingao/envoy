#pragma once

#include <memory>
#include <string>
#include <vector>

#include "source/common/common/assert.h"
#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"

#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

class FilterManager;
class FieldStreamingSession;

// `AiRequest` represents the structured request payload presented to an `AiFilter`.
// It wraps a JsonWithExtBuf document and manages registration of field paths
// that the filter wants to stream.
class AiRequest {
public:
  explicit AiRequest(JsonWithExtBuf doc, FilterManager& manager, size_t filter_index = 0);
  ~AiRequest() = default;

  AiRequest(AiRequest&&) noexcept = default;
  AiRequest& operator=(AiRequest&&) = delete;
  AiRequest(const AiRequest&) = delete;
  AiRequest& operator=(const AiRequest&) = delete;

  // Document access
  const JsonWithExtBuf& doc() const { return doc_; }
  JsonWithExtBuf& doc() { return doc_; }

  // Stream registration: records interest in streaming a JSON path
  void registerFieldForStreaming(absl::string_view path);

  // Starts a filter streaming stage ending at this filter.
  // Captures current registered paths and clears registered_paths_ for subsequent stages.
  std::unique_ptr<FieldStreamingSession> stream();

private:
  friend class FilterManager;

  const std::vector<std::string>& registeredPaths() const { return registered_paths_; }
  void clearStreamInterests() { registered_paths_.clear(); }
  size_t filterIndex() const { return filter_index_; }
  void setFilterIndex(size_t index) { filter_index_ = index; }

  JsonWithExtBuf doc_;
  FilterManager& manager_;
  size_t filter_index_{0};
  std::vector<std::string> registered_paths_;
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
