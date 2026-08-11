#pragma once

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

struct FieldSchema;

enum class FieldKind {
  String,
  Number,
  // A number with no fractional part. 1024.0 is one, 1.5 is not.
  Int,
  Bool,
  // Must be an object; its contents are checked against `fields`.
  Object,
  Array,
  // Any JSON of any type, never descended into. Unlike an Object with no declared
  // fields, this does not even require an object -- it is for caller-authored JSON
  // Schema, which a proxy has no business interpreting.
  AnyJson,
  OneOf,
};

// The edge from a parent object to a child schema. `required` lives here, not on
// the node, so one node can be shared by several parents (the content-part schema
// is reached from both messages[].content and prediction.content).
struct ObjectField {
  const FieldSchema* schema{nullptr};
  bool required{false};
};

// Keys are string literals from the declaration; values point into the builder's
// arena, so rehashing never moves a node.
using ObjectFields = absl::flat_hash_map<absl::string_view, ObjectField>;

// One node of a schema tree. Only the members its kind names are meaningful.
// SchemaBuilder is the only way to make one.
//
// Built bottom-up -- a parent needs its children's pointers -- so the result is a
// DAG and a walk of it terminates.
struct FieldSchema {
  FieldKind kind{FieldKind::AnyJson};

  // String only: whether the value may be left in the external buffer instead of
  // materialized in the DOM.
  //
  // Declared here but not acted on yet: the parser still offloads any string over
  // its inline threshold, whatever the schema says.
  // TODO(penguingao): have the parser consult this, so a field something needs to
  // read stays in the DOM and one nothing reads can leave it regardless of size.
  // That needs a path-keyed projection of the tree, since the parser decides per
  // value from the cursor's pattern path rather than from a node.
  bool offloadable{false};

  // Number/Int: inclusive bounds; unset means unbounded. Not named min/max, which
  // are macros in windows.h.
  std::optional<double> min_value;
  std::optional<double> max_value;

  // Array.
  std::size_t min_items{0};
  const FieldSchema* element{nullptr}; // Null leaves elements unconstrained.

  // Object. A key not in `fields` passes untouched: this is a proxy, not a
  // validating gateway.
  ObjectFields fields;
  // Derived by the builder, so the walk can spot a missing required field with a
  // counter instead of a second pass.
  std::size_t required_field_count{0};

  // OneOf: semantically anyOf -- the first alternative that validates wins.
  std::vector<const FieldSchema*> alternatives;
};

// Tag making a required field read as one at the declaration site.
struct RequiredTag {};
constexpr RequiredTag Required{};

// One entry of an object declaration. Two constructors, so an optional field is
// `{"temperature", b.number(0, 2)}` and a required one `{"model", Required,
// b.str()}` -- no `Optional` noise on the many optional ones.
struct FieldDecl {
  FieldDecl(absl::string_view name, const FieldSchema* schema)
      : name(name), schema(schema), required(false) {}
  FieldDecl(absl::string_view name, RequiredTag, const FieldSchema* schema)
      : name(name), schema(schema), required(true) {}

  absl::string_view name;
  const FieldSchema* schema;
  bool required;
};

// Builds schema nodes and owns them.
//
// LIFETIME: every node is separately heap-allocated and never moved, so a pointer
// handed back stays valid for the builder's lifetime -- including across a move of
// the builder, which moves the unique_ptrs and not the nodes. A builder is only
// ever a member of a process-lifetime object (tree_payload_schema.h).
class SchemaBuilder {
public:
  const FieldSchema* str();
  // A string that may stay in the external buffer.
  const FieldSchema* offloadableStr();
  const FieldSchema* number(std::optional<double> min_value = std::nullopt,
                            std::optional<double> max_value = std::nullopt);
  const FieldSchema* integer(std::optional<double> min_value = std::nullopt,
                             std::optional<double> max_value = std::nullopt);
  const FieldSchema* boolean();
  const FieldSchema* anyJson();
  const FieldSchema* object(std::initializer_list<FieldDecl> fields);
  // Must be an object; contents unconstrained.
  const FieldSchema* anyObject() { return object({}); }
  const FieldSchema* array(const FieldSchema* element, std::size_t min_items = 0);
  const FieldSchema* oneOf(std::initializer_list<const FieldSchema*> alternatives);

private:
  const FieldSchema* intern(FieldSchema&& node);

  std::vector<std::unique_ptr<FieldSchema>> nodes_;
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
