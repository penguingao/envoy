#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "source/common/common/assert.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

nlohmann::json ExtBufLocation::toBinary(const ExtBufLocation& loc) {
  std::vector<uint8_t> bytes(sizeof(ExtBufLocation));
  std::memcpy(bytes.data(), &loc, sizeof(ExtBufLocation));
  return nlohmann::json::binary(std::move(bytes), kSubtypeOffloaded);
}

std::optional<ExtBufLocation> ExtBufLocation::fromBinary(const nlohmann::json& j) {
  if (!j.is_binary()) {
    return std::nullopt;
  }
  const auto& bin = j.get_binary();
  if (!bin.has_subtype() || bin.subtype() != kSubtypeOffloaded) {
    return std::nullopt;
  }
  if (bin.size() < sizeof(ExtBufLocation)) {
    return std::nullopt;
  }
  ExtBufLocation loc;
  std::memcpy(&loc, bin.data(), sizeof(ExtBufLocation));
  return loc;
}

bool JsonWithExtBuf::isOffloaded(const nlohmann::json& node) {
  if (!node.is_binary()) {
    return false;
  }
  const auto& bin = node.get_binary();
  return bin.has_subtype() && bin.subtype() == ExtBufLocation::kSubtypeOffloaded;
}

std::optional<ExtBufLocation> JsonWithExtBuf::getExtBufLocation(const nlohmann::json& node) {
  ASSERT(isOffloaded(node));
  return ExtBufLocation::fromBinary(node);
}

nlohmann::json JsonWithExtBuf::makeOffloadedRef(uint64_t offset, uint64_t length) {
  ExtBufLocation loc{offset, length};
  return ExtBufLocation::toBinary(loc);
}

JsonWithExtBufParser::JsonWithExtBufParser(JsonWithExtBufParserConfig config)
    : config_(std::move(config)), cursor_(*this, config_.track_paths) {}

absl::Status JsonWithExtBufParser::feed(absl::string_view chunk, bool is_last) {
  if (finalized_) {
    return absl::FailedPreconditionError("Cannot feed chunk into finalized parser");
  }
  return cursor_.feed(chunk, is_last);
}

absl::StatusOr<std::unique_ptr<JsonWithExtBuf>> JsonWithExtBufParser::finalize() {
  if (!stack_.empty()) {
    return absl::InvalidArgumentError("Unbalanced JSON document: unclosed containers remaining");
  }
  finalized_ = true;
  return std::make_unique<JsonWithExtBuf>(std::move(root_));
}

void JsonWithExtBufParser::attachValue(nlohmann::json val) {
  if (stack_.empty()) {
    root_ = std::move(val);
    return;
  }
  auto& top = stack_.back();
  if (top.is_dict) {
    (*top.node)[top.pending_key] = std::move(val);
    top.pending_key.clear();
  } else {
    top.node->push_back(std::move(val));
  }
}

bool JsonWithExtBufParser::openStringCapture(absl::string_view key, int depth, size_t token_start) {
  current_token_start_ = token_start;
  current_string_buffer_.clear();

  if (config_.should_offload_key && config_.should_offload_key(key, depth)) {
    if (config_.min_cutoff_size == 0) {
      offloading_current_string_ = true;
      offload_candidate_ = false;
      return false; // Skip unescaping/accumulating chunks in memory
    }
    offloading_current_string_ = false;
    offload_candidate_ = true;
    return true; // Accumulate string in memory to check cutoff at end
  }

  offloading_current_string_ = false;
  offload_candidate_ = false;
  return true; // Inlined string
}

bool JsonWithExtBufParser::onStringChunk(absl::string_view /*key*/, int /*depth*/,
                                         absl::string_view chunk) {
  current_string_buffer_.append(chunk.data(), chunk.size());
  return true;
}

void JsonWithExtBufParser::closeStringCapture(absl::string_view /*key*/, int /*depth*/,
                                              size_t token_end) {
  size_t token_length =
      (token_end >= current_token_start_) ? (token_end - current_token_start_) : 0;
  bool should_offload =
      offloading_current_string_ || (offload_candidate_ && token_length >= config_.min_cutoff_size);

  if (should_offload) {
    // token_start is opening quote, token_end is after closing quote.
    uint64_t content_offset = current_token_start_ + 1;
    uint64_t content_length = (token_length >= 2) ? (token_length - 2) : 0;
    attachValue(JsonWithExtBuf::makeOffloadedRef(content_offset, content_length));
  } else {
    attachValue(nlohmann::json(current_string_buffer_));
  }

  offloading_current_string_ = false;
  offload_candidate_ = false;
  current_string_buffer_.clear();
}

absl::Status JsonWithExtBufParser::onKey(absl::string_view key, int /*depth*/,
                                         size_t /*token_start*/) {
  if (stack_.empty() || !stack_.back().is_dict) {
    return absl::InvalidArgumentError("Key encountered outside of an object container");
  }
  auto& top = stack_.back();
  if (top.node->contains(key)) {
    return absl::InvalidArgumentError(absl::StrCat("Duplicate JSON key: ", key));
  }
  top.pending_key = std::string(key);
  return absl::OkStatus();
}

absl::Status JsonWithExtBufParser::onNumber(absl::string_view /*key*/, absl::string_view raw,
                                            int /*depth*/, size_t /*token_start*/,
                                            size_t /*token_end*/) {
  int64_t int_val;
  if (absl::SimpleAtoi(raw, &int_val)) {
    attachValue(nlohmann::json(int_val));
    return absl::OkStatus();
  }
  uint64_t uint_val;
  if (absl::SimpleAtoi(raw, &uint_val)) {
    attachValue(nlohmann::json(uint_val));
    return absl::OkStatus();
  }
  double dbl_val;
  if (absl::SimpleAtod(raw, &dbl_val)) {
    attachValue(nlohmann::json(dbl_val));
    return absl::OkStatus();
  }
  return absl::InvalidArgumentError(absl::StrCat("Invalid number literal: ", raw));
}

absl::Status JsonWithExtBufParser::onBoolean(absl::string_view /*key*/, bool value, int /*depth*/,
                                             size_t /*token_start*/, size_t /*token_end*/) {
  attachValue(nlohmann::json(value));
  return absl::OkStatus();
}

void JsonWithExtBufParser::onNull(absl::string_view /*key*/, int /*depth*/, size_t /*token_start*/,
                                  size_t /*token_end*/) {
  attachValue(nlohmann::json(nullptr));
}

void JsonWithExtBufParser::onContainerOpen(absl::string_view /*key*/, bool is_dict, int /*depth*/,
                                           size_t /*token_start*/) {
  nlohmann::json new_container = is_dict ? nlohmann::json::object() : nlohmann::json::array();
  if (stack_.empty()) {
    root_ = std::move(new_container);
    stack_.push_back({&root_, "", is_dict});
    return;
  }
  auto& top = stack_.back();
  if (top.is_dict) {
    (*top.node)[top.pending_key] = std::move(new_container);
    nlohmann::json& child = (*top.node)[top.pending_key];
    top.pending_key.clear();
    stack_.push_back({&child, "", is_dict});
  } else {
    top.node->push_back(std::move(new_container));
    nlohmann::json& child = top.node->back();
    stack_.push_back({&child, "", is_dict});
  }
}

void JsonWithExtBufParser::onContainerClose(int /*depth*/, size_t /*token_end*/) {
  if (!stack_.empty()) {
    stack_.pop_back();
  }
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
