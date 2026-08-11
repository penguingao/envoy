#include "source/extensions/filters/http/ai_protocol_manager/schema/offload_plan.h"

#include <algorithm>

#include "source/common/common/assert.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

// Extends `parent`, matching buildPatternPath(): no separator at the root.
std::string fieldPath(absl::string_view parent, absl::string_view name) {
  return parent.empty() ? std::string(name) : absl::StrCat(parent, ".", name);
}

// Collects the path of every declared string field.
//
// A shared node is visited once per parent that reaches it, which is the point:
// one content-part schema describes both messages[].content[].text and
// prediction.content[].text.
//
// Only String nodes count. A number is never offloaded, and an AnyJson subtree is
// deliberately skipped so strings inside a caller's tool schema read as
// undeclared, hence offloadable.
void collect(const FieldSchema& schema, absl::string_view path,
             absl::flat_hash_set<std::string>& offloadable,
             absl::flat_hash_set<std::string>& inline_only) {
  switch (schema.kind) {
  case FieldKind::String:
    (schema.offloadable ? offloadable : inline_only).insert(std::string(path));
    return;
  case FieldKind::Object:
    for (const auto& [name, field] : schema.fields) {
      collect(*field.schema, fieldPath(path, name), offloadable, inline_only);
    }
    return;
  case FieldKind::Array:
    if (schema.element != nullptr) {
      collect(*schema.element, absl::StrCat(path, "[]"), offloadable, inline_only);
    }
    return;
  case FieldKind::OneOf:
    // Alternatives share the path, which is why both the string and parts-array
    // forms of a content field end up described.
    for (const FieldSchema* alternative : schema.alternatives) {
      collect(*alternative, path, offloadable, inline_only);
    }
    return;
  case FieldKind::Number:
  case FieldKind::Int:
  case FieldKind::Bool:
  case FieldKind::AnyJson:
    return;
  }
}

} // namespace

OffloadPlan::OffloadPlan(const FieldSchema& root,
                         absl::Span<const absl::string_view> stream_order) {
  collect(root, /*path=*/"", offloadable_paths_, inline_only_paths_);

  // A field reachable as both keeps the stricter answer: something may need to
  // read it.
  for (const std::string& path : inline_only_paths_) {
    offloadable_paths_.erase(path);
  }

  for (const absl::string_view path : stream_order) {
    RELEASE_ASSERT(offloadable_paths_.contains(path),
                   absl::StrCat("schema: stream order names '", path,
                                "', which is not a declared offloadable field"));
    stream_order_.emplace_back(path);
  }

  // Offloadable fields the order does not name go after it, sorted so the result
  // does not depend on hash iteration.
  std::vector<std::string> unlisted;
  for (const std::string& path : offloadable_paths_) {
    if (std::find(stream_order_.begin(), stream_order_.end(), path) == stream_order_.end()) {
      unlisted.push_back(path);
    }
  }
  std::sort(unlisted.begin(), unlisted.end());
  stream_order_.insert(stream_order_.end(), unlisted.begin(), unlisted.end());
}

bool OffloadPlan::isOffloadable(absl::string_view pattern_path) const {
  // Undeclared paths fall through to true; only a declared inline-only field is
  // held back.
  return !inline_only_paths_.contains(pattern_path);
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
