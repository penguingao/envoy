#pragma once

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

// WuffsJsonObjectReader: one-shot, allocation-free JSON object parser built on
// the Wuffs JSON tokenizer (https://github.com/google/wuffs).
//
// Parses a JSON object at the root level and fires Handler callbacks for each
// top-level key-value pair.  Nested objects/arrays are returned as raw JSON
// byte slices (zero-copy from the input buffer) rather than recursively parsed,
// which is sufficient for routing-field extraction.
//
// Memory model: no heap allocation beyond the Handler's own state.  The Wuffs
// decoder is stack-allocated; token and source buffers point into the input.
class WuffsJsonObjectReader {
public:
  struct Handler {
    virtual ~Handler() = default;

    // String-valued top-level field.  value is the decoded string (escape
    // sequences resolved; no surrounding quotes).
    virtual void onStringField(absl::string_view /*key*/, std::string /*value*/) {}

    // Object-valued top-level field.  raw_json is a slice of the input
    // covering the full object including the surrounding { }.
    virtual void onObjectField(absl::string_view /*key*/, absl::string_view /*raw_json*/) {}

    // Array-valued top-level field.  raw_json is a slice of the input
    // covering the full array including the surrounding [ ].
    virtual void onArrayField(absl::string_view /*key*/, absl::string_view /*raw_json*/) {}

    // Scalar top-level fields (less common in params objects; provided for completeness).
    virtual void onIntField(absl::string_view /*key*/, int64_t /*value*/) {}
    virtual void onFloatField(absl::string_view /*key*/, double /*value*/) {}
    virtual void onBoolField(absl::string_view /*key*/, bool /*value*/) {}
    virtual void onNullField(absl::string_view /*key*/) {}
  };

  // Parses `input` and fires callbacks on `handler`.
  // Returns non-OK if the input is not valid JSON or is not a JSON object.
  static absl::Status read(absl::string_view input, Handler& handler);
};

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
