#pragma once

#include <string>
#include <vector>

#include "source/extensions/filters/http/ai_protocol_manager/schema/field_schema.h"

#include "absl/container/flat_hash_set.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// Which of a schema's string fields may be left in the external buffer, and the
// order the filter chain streams them in.
//
// Derived from the tree rather than declared separately, so the schema stays the
// single declaration of what a field is.
class OffloadPlan {
public:
  // `stream_order` lists offloadable fields, outermost-first, in the order they
  // should stream -- a path per entry, in buildPatternPath() form. Offloadable
  // fields it omits stream after those it names, ordered by path.
  //
  // Every listed path must be a declared offloadable field of `root`; a typo
  // would otherwise silently sink a field to the end, so it is asserted.
  OffloadPlan(const FieldSchema& root, absl::Span<const absl::string_view> stream_order);

  // Whether a string at `pattern_path` may be left in the external buffer:
  //
  //   declared offloadable      -> true
  //   declared, not offloadable -> false; something may need to read it
  //   not declared at all       -> true; an undeclared field is pass-through, so
  //                                nothing constrains it
  bool isOffloadable(absl::string_view pattern_path) const;

  // Offloadable fields in streaming order.
  absl::Span<const std::string> streamOrder() const { return stream_order_; }

private:
  std::vector<std::string> stream_order_;
  absl::flat_hash_set<std::string> offloadable_paths_;
  // Declared string fields that must stay inline. Only these make isOffloadable()
  // false.
  absl::flat_hash_set<std::string> inline_only_paths_;
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
