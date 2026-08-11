#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "source/extensions/filters/http/ai_protocol_manager/schema/field_schema.h"

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// One offloadable field of a schema.
struct OffloadSpec {
  // The field's path in WuffsJsonCursor::buildPatternPath() form, e.g.
  // "messages[].content[].text". That form is the contract with the parser: it is
  // what the cursor can produce cheaply mid-parse, when the offload decision has
  // to be made -- before the value is even complete.
  std::string pattern_path;
  std::uint32_t stream_order{StreamOrder::Other};
};

// Which of a schema's string fields may be left in the external buffer, and in
// what order the filter chain streams them.
//
// Derived from a FieldSchema tree rather than declared separately, so the schema
// stays the single declaration and the two cannot drift.
class OffloadPlan {
public:
  explicit OffloadPlan(const FieldSchema& root);

  // Whether a string at `pattern_path` may be left in the external buffer.
  // Three cases, and the middle one is the point of the whole class:
  //
  //   declared offloadable        -> true
  //   declared, not offloadable   -> false; something has to be able to read it
  //   not declared at all         -> true; an undeclared field is pass-through,
  //                                  so nothing constrains it and keeping a large
  //                                  one out of the DOM costs nothing
  bool isOffloadable(absl::string_view pattern_path) const;

  // The spec for a declared offloadable field, or nullptr. An undeclared field is
  // offloadable but has no spec; it streams at StreamOrder::UndeclaredField.
  const OffloadSpec* find(absl::string_view pattern_path) const;

  // Declared offloadable fields in streaming order, lowest rank first. Ties break
  // on path so the order is deterministic. Consumed by the streaming filter API.
  absl::Span<const OffloadSpec> streamOrder() const { return specs_; }

private:
  // Sorted by (stream_order, pattern_path) at construction and never modified
  // after, so the pointers into it below stay valid.
  std::vector<OffloadSpec> specs_;
  absl::flat_hash_map<absl::string_view, const OffloadSpec*> by_path_;
  // Declared string fields that must stay inline. Only these make isOffloadable()
  // false; everything else the schema does not mention is fair game.
  absl::flat_hash_set<std::string> inline_only_paths_;
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
