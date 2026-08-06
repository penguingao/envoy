#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "source/common/json/wuffs_json/wuffs_json_cursor.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer.h"

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

/**
 * Metadata stored in nlohmann::json binary subtype fields to locate offloaded
 * buffer chunks within an ExternalBuffer.
 */
struct ExtBufLocation {
  // Byte offset of the full token in the ExternalBuffer (including surrounding quotes for strings).
  uint64_t token_offset{0};
  // Byte length of the full token in the ExternalBuffer.
  uint64_t token_length{0};
  // Byte offset of the payload/inner content in the ExternalBuffer (excluding quotes for strings).
  uint64_t content_offset{0};
  // Byte length of the inner content.
  uint64_t content_length{0};
  // Subtype identifying the category of offloaded payload.
  uint8_t subtype{kSubtypeOffloadedString};

  // Subtype constants
  static constexpr uint8_t kSubtypeOffloadedString = 0x01;
  static constexpr uint8_t kSubtypeOffloadedRawJson = 0x02;
  static constexpr uint8_t kSubtypeOffloadedBinary = 0x03;

  /**
   * Serializes an ExtBufLocation into a binary nlohmann::json node with the given subtype.
   */
  static nlohmann::json toBinary(const ExtBufLocation& loc,
                                 uint8_t subtype = kSubtypeOffloadedString);

  /**
   * Deserializes an ExtBufLocation from a binary nlohmann::json node if valid.
   */
  static std::optional<ExtBufLocation> fromBinary(const nlohmann::json& j);
};

/**
 * Base class for structured JSON documents that reference large payloads offloaded
 * to an ExternalBuffer.
 */
class JsonWithExtBuf {
public:
  JsonWithExtBuf() = default;
  explicit JsonWithExtBuf(nlohmann::json root, std::shared_ptr<ExternalBuffer> ext_buf = nullptr);
  virtual ~JsonWithExtBuf() = default;

  // External buffer access
  std::shared_ptr<ExternalBuffer> externalBuffer() const { return external_buffer_; }
  void setExternalBuffer(std::shared_ptr<ExternalBuffer> ext_buf) {
    external_buffer_ = std::move(ext_buf);
  }

  // DOM access
  nlohmann::json& json() { return root_; }
  const nlohmann::json& json() const { return root_; }

  nlohmann::json& operator[](absl::string_view key) { return root_[std::string(key)]; }
  const nlohmann::json& operator[](absl::string_view key) const { return root_[std::string(key)]; }

  /**
   * Returns true if the given JSON node represents an offloaded buffer slice.
   */
  static bool isOffloaded(const nlohmann::json& node);

  /**
   * Extracts location metadata from an offloaded binary JSON node.
   */
  static std::optional<ExtBufLocation> getExtBufLocation(const nlohmann::json& node);

  /**
   * Constructs an offloaded binary JSON node pointing to an external buffer location.
   */
  static nlohmann::json makeOffloadedRef(uint64_t token_offset, uint64_t token_length,
                                         uint64_t content_offset = 0, uint64_t content_length = 0,
                                         uint8_t subtype = ExtBufLocation::kSubtypeOffloadedString);

  /**
   * Predicate callback to determine if a string at (key, depth, token_start) should be
   * offloaded to ExternalBuffer or inlined into the JSON DOM.
   */
  using OffloadPredicate =
      absl::AnyInvocable<bool(absl::string_view key, int depth, size_t token_start) const>;

  /**
   * Parses complete JSON data into a JsonWithExtBuf using WuffsJsonCursor.
   */
  static absl::StatusOr<std::unique_ptr<JsonWithExtBuf>>
  parse(absl::string_view json_data, std::shared_ptr<ExternalBuffer> ext_buf = nullptr,
        OffloadPredicate offload_predicate = nullptr);

protected:
  nlohmann::json root_{nlohmann::json::object()};
  std::shared_ptr<ExternalBuffer> external_buffer_;
};

/**
 * SAX-style builder implementing WuffsJsonCursor::Handler to stream-parse JSON
 * into JsonWithExtBuf while offloading designated string fields.
 */
class JsonWithExtBufParser : public Json::Wuffs::WuffsJsonCursor::Handler {
public:
  using OffloadPredicate = JsonWithExtBuf::OffloadPredicate;

  explicit JsonWithExtBufParser(std::shared_ptr<ExternalBuffer> ext_buf = nullptr,
                                OffloadPredicate offload_predicate = nullptr,
                                bool track_paths = false);
  ~JsonWithExtBufParser() override = default;

  /**
   * Feeds a chunk of raw JSON bytes into the streaming parser.
   */
  absl::Status feed(absl::string_view chunk, bool is_last);

  /**
   * Finalizes parsing and returns the resulting JsonWithExtBuf instance.
   */
  absl::StatusOr<std::unique_ptr<JsonWithExtBuf>> finalize();

  // WuffsJsonCursor::Handler implementation
  bool openStringCapture(absl::string_view key, int depth, size_t token_start) override;
  bool onStringChunk(absl::string_view key, int depth, absl::string_view chunk) override;
  void closeStringCapture(absl::string_view key, int depth, size_t token_end) override;
  absl::Status onKey(absl::string_view key, int depth, size_t token_start) override;
  absl::Status onNumber(absl::string_view key, absl::string_view raw, int depth, size_t token_start,
                        size_t token_end) override;
  absl::Status onBoolean(absl::string_view key, bool value, int depth, size_t token_start,
                         size_t token_end) override;
  void onNull(absl::string_view key, int depth, size_t token_start, size_t token_end) override;
  void onContainerOpen(absl::string_view key, bool is_dict, int depth, size_t token_start) override;
  void onContainerClose(int depth, size_t token_end) override;

private:
  struct StackFrame {
    nlohmann::json* node{nullptr};
    std::string pending_key;
    bool is_dict{false};
  };

  void attachValue(nlohmann::json val);

  std::shared_ptr<ExternalBuffer> external_buffer_;
  OffloadPredicate offload_predicate_;
  Json::Wuffs::WuffsJsonCursor cursor_;

  nlohmann::json root_{nullptr};
  std::vector<StackFrame> stack_;

  bool offloading_current_string_{false};
  size_t current_token_start_{0};
  std::string current_string_buffer_;
  bool finalized_{false};
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
