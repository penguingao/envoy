#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

nlohmann::json ExtBufLocation::toBinary(const ExtBufLocation& loc, uint8_t subtype) {
  std::vector<uint8_t> bytes(sizeof(ExtBufLocation));
  std::memcpy(bytes.data(), &loc, sizeof(ExtBufLocation));
  return nlohmann::json::binary(std::move(bytes), subtype);
}

std::optional<ExtBufLocation> ExtBufLocation::fromBinary(const nlohmann::json& j) {
  if (!j.is_binary()) {
    return std::nullopt;
  }
  const auto& bin = j.get_binary();
  if (bin.size() < sizeof(ExtBufLocation)) {
    return std::nullopt;
  }
  ExtBufLocation loc;
  std::memcpy(&loc, bin.data(), sizeof(ExtBufLocation));
  if (bin.has_subtype()) {
    loc.subtype = bin.subtype();
  }
  return loc;
}

JsonWithExtBuf::JsonWithExtBuf(nlohmann::json root, std::shared_ptr<ExternalBuffer> ext_buf)
    : root_(std::move(root)), external_buffer_(std::move(ext_buf)) {}

bool JsonWithExtBuf::isOffloaded(const nlohmann::json& node) {
  if (!node.is_binary()) {
    return false;
  }
  const auto& bin = node.get_binary();
  if (!bin.has_subtype()) {
    return false;
  }
  uint8_t sub = bin.subtype();
  return sub == ExtBufLocation::kSubtypeOffloadedString ||
         sub == ExtBufLocation::kSubtypeOffloadedRawJson ||
         sub == ExtBufLocation::kSubtypeOffloadedBinary;
}

std::optional<ExtBufLocation> JsonWithExtBuf::getExtBufLocation(const nlohmann::json& node) {
  return ExtBufLocation::fromBinary(node);
}

nlohmann::json JsonWithExtBuf::makeOffloadedRef(uint64_t token_offset, uint64_t token_length,
                                                uint64_t content_offset, uint64_t content_length,
                                                uint8_t subtype) {
  ExtBufLocation loc{token_offset, token_length, content_offset, content_length, subtype};
  return ExtBufLocation::toBinary(loc, subtype);
}

absl::StatusOr<std::unique_ptr<JsonWithExtBuf>>
JsonWithExtBuf::parse(absl::string_view json_data, std::shared_ptr<ExternalBuffer> ext_buf,
                      OffloadPredicate offload_predicate) {
  JsonWithExtBufParser parser(std::move(ext_buf), std::move(offload_predicate));
  auto status = parser.feed(json_data, /*is_last=*/true);
  if (!status.ok()) {
    return status;
  }
  return parser.finalize();
}

JsonWithExtBufParser::JsonWithExtBufParser(std::shared_ptr<ExternalBuffer> ext_buf,
                                           OffloadPredicate offload_predicate, bool track_paths)
    : external_buffer_(std::move(ext_buf)), offload_predicate_(std::move(offload_predicate)),
      cursor_(*this, track_paths) {}

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
  return std::make_unique<JsonWithExtBuf>(std::move(root_), external_buffer_);
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

  if (offload_predicate_ && offload_predicate_(key, depth, token_start)) {
    offloading_current_string_ = true;
    return false; // Skip unescaping/accumulating chunks in memory
  }

  offloading_current_string_ = false;
  return true; // Accumulate unescaped chunks
}

bool JsonWithExtBufParser::onStringChunk(absl::string_view /*key*/, int /*depth*/,
                                         absl::string_view chunk) {
  current_string_buffer_.append(chunk.data(), chunk.size());
  return true;
}

void JsonWithExtBufParser::closeStringCapture(absl::string_view /*key*/, int /*depth*/,
                                              size_t token_end) {
  if (offloading_current_string_) {
    uint64_t token_length =
        (token_end >= current_token_start_) ? (token_end - current_token_start_) : 0;
    // token_start is opening quote, token_end is after closing quote.
    uint64_t content_offset = current_token_start_ + 1;
    uint64_t content_length = (token_length >= 2) ? (token_length - 2) : 0;

    ExtBufLocation loc{current_token_start_, token_length, content_offset, content_length,
                       ExtBufLocation::kSubtypeOffloadedString};
    attachValue(ExtBufLocation::toBinary(loc, loc.subtype));
  } else {
    attachValue(nlohmann::json(current_string_buffer_));
    current_string_buffer_.clear();
  }
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
