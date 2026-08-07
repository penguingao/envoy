#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "source/common/common/assert.h"
#include "source/common/json/wuffs_json/wuffs_json_cursor.h"

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
 * buffer chunks within the payload buffer.
 */
struct ExtBufLocation {
  // Byte offset of the content in the payload buffer.
  uint64_t offset{0};
  // Byte length of the content in the payload buffer.
  uint64_t length{0};

  static constexpr uint8_t kSubtypeOffloaded = 0x01;

  /**
   * Serializes an ExtBufLocation into a binary nlohmann::json node.
   */
  static nlohmann::json toBinary(const ExtBufLocation& loc);

  /**
   * Deserializes an ExtBufLocation from a binary nlohmann::json node if valid.
   * If the binary subtype does not match kSubtypeOffloaded, returns nullopt immediately
   * without parsing.
   */
  static std::optional<ExtBufLocation> fromBinary(const nlohmann::json& j);
};

/**
 * Base class for structured JSON documents that reference large payloads offloaded
 * to an external buffer.
 */
class JsonWithExtBuf {
public:
  JsonWithExtBuf() = default;
  explicit JsonWithExtBuf(nlohmann::json root) : root_(std::move(root)) {}
  virtual ~JsonWithExtBuf() = default;

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
   * Asserts that the node is an offloaded node.
   */
  static std::optional<ExtBufLocation> getExtBufLocation(const nlohmann::json& node);

  /**
   * Constructs an offloaded binary JSON node pointing to a buffer location.
   */
  static nlohmann::json makeOffloadedRef(uint64_t offset, uint64_t length);

protected:
  nlohmann::json root_{nlohmann::json::object()};
};

/**
 * Configuration options for the streaming JSON parser.
 */
struct JsonWithExtBufParserConfig {
  // Minimum token length (in raw JSON bytes) required to trigger offloading.
  // String tokens shorter than this cutoff are inlined into the JSON DOM.
  // A cutoff of 0 means all matching strings are offloaded.
  size_t min_cutoff_size{0};

  // Predicate callback to determine if a string under key at depth should be considered for
  // offloading.
  using KeyFilter = absl::AnyInvocable<bool(absl::string_view key, int depth) const>;
  KeyFilter should_offload_key;

  bool track_paths{false};
};

/**
 * SAX-style builder implementing WuffsJsonCursor::Handler to stream-parse JSON
 * into JsonWithExtBuf while offloading designated string fields.
 */
class JsonWithExtBufParser : public Json::Wuffs::WuffsJsonCursor::Handler {
public:
  explicit JsonWithExtBufParser(JsonWithExtBufParserConfig config = {});
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

  JsonWithExtBufParserConfig config_;
  Json::Wuffs::WuffsJsonCursor cursor_;

  nlohmann::json root_{nullptr};
  std::vector<StackFrame> stack_;

  bool offloading_current_string_{false};
  bool offload_candidate_{false};
  size_t current_token_start_{0};
  std::string current_string_buffer_;
  bool finalized_{false};
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
