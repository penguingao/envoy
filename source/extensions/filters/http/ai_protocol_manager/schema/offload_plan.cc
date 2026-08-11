#include "source/extensions/filters/http/ai_protocol_manager/schema/offload_plan.h"

#include <algorithm>
#include <tuple>
#include <utility>

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

// Extends `parent` with an object field, matching buildPatternPath(): the
// separator is omitted at the root, where there is nothing to separate from.
std::string fieldPath(absl::string_view parent, absl::string_view name) {
  return parent.empty() ? std::string(name) : absl::StrCat(parent, ".", name);
}

// Walks the tree collecting the path of every declared string field.
//
// A shared node is visited once per parent that reaches it, which is the point:
// the content-part schema reached from messages[].content[] and from
// prediction.content[] is one node describing two distinct paths.
//
// Only String nodes are collected. A number or boolean is never offloaded, so its
// path would change no decision, and an AnyJson subtree is deliberately left out
// -- strings inside a caller's tool schema are undeclared, hence offloadable,
// which is what keeps a large tool description out of the DOM.
void collect(const FieldSchema& schema, absl::string_view path,
             std::vector<OffloadSpec>& offloadable, absl::flat_hash_set<std::string>& inline_only) {
  switch (schema.kind) {
  case FieldKind::String:
    if (schema.offloadable) {
      // A path reachable as both offloadable and inline-only resolves to
      // inline-only below: the field that has to be read wins.
      offloadable.push_back(OffloadSpec{std::string(path), schema.stream_order});
    } else {
      inline_only.insert(std::string(path));
    }
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
    // Every alternative sits at the same path, which is exactly why the string
    // and the parts-array forms of messages[].content both end up described.
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

OffloadPlan::OffloadPlan(const FieldSchema& root) {
  std::vector<OffloadSpec> offloadable;
  collect(root, /*path=*/"", offloadable, inline_only_paths_);

  // A field something has to read wins over one that may be offloaded, so a path
  // declared both ways stays inline.
  for (OffloadSpec& spec : offloadable) {
    if (!inline_only_paths_.contains(spec.pattern_path)) {
      specs_.push_back(std::move(spec));
    }
  }

  // Streaming order, with the path breaking ties so the order does not depend on
  // hash iteration.
  std::sort(specs_.begin(), specs_.end(), [](const OffloadSpec& a, const OffloadSpec& b) {
    return std::tie(a.stream_order, a.pattern_path) < std::tie(b.stream_order, b.pattern_path);
  });

  by_path_.reserve(specs_.size());
  for (const OffloadSpec& spec : specs_) {
    by_path_.emplace(spec.pattern_path, &spec);
  }
}

const OffloadSpec* OffloadPlan::find(absl::string_view pattern_path) const {
  const auto it = by_path_.find(pattern_path);
  return it == by_path_.end() ? nullptr : it->second;
}

bool OffloadPlan::isOffloadable(absl::string_view pattern_path) const {
  if (by_path_.contains(pattern_path)) {
    return true;
  }
  // Undeclared paths fall through to true; only a declared inline-only field is
  // held back.
  return !inline_only_paths_.contains(pattern_path);
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
