#pragma once

#include <cstddef>
#include <cstdint>
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

// What a value must be.
enum class FieldKind {
  String,
  Number,
  // A number with no fractional part. 1024.0 is one, 1.5 is not.
  Int,
  Bool,
  Object,
  Array,
  // Opaque: any well-formed JSON, never descended into. For caller-authored JSON
  // Schema (tools[].function.parameters, response_format.json_schema.schema),
  // which a proxy has no business modelling.
  AnyJson,
  OneOf,
};

// The edge from a parent object to a child schema.
//
// `required` lives here rather than on the node because requiredness is a
// property of the field's place in its parent, not of the value's shape -- which
// is what lets one node be shared by several parents (the content-part schema is
// reached from both messages[].content and prediction.content).
struct ObjectField {
  const FieldSchema* schema{nullptr};
  bool required{false};
};

// The declared fields of an object.
//
// Keys are the string literals from the schema declaration, so the views outlive
// any lookup. Values are pointers into the builder's arena, so growing or
// rehashing this map never moves a node.
using ObjectFields = absl::flat_hash_map<absl::string_view, ObjectField>;

// Rank in the order the AI filter chain streams offloaded buffers, lowest first.
// The order the design intends is prompts, then tools, then everything else.
//
// Named rather than bare integers so a new field is placed relative to the
// existing ones, and spaced so one can be slotted between two without
// renumbering the rest.
namespace StreamOrder {
constexpr std::uint32_t Prompt = 100;
constexpr std::uint32_t Tool = 200;
constexpr std::uint32_t Other = 300;
// Anything the schema does not declare. Streams last: a field nothing is
// declared about is a field nothing is waiting on.
constexpr std::uint32_t UndeclaredField = 1000;
} // namespace StreamOrder

// One node of a schema tree. Only the members its kind names are meaningful.
//
// SchemaBuilder is the only way to make one, and it asserts that invariant at
// construction time.
//
// The tree is built strictly bottom-up -- a parent can only be declared once its
// children have pointers -- so it is a DAG by construction and a walk of it
// terminates.
struct FieldSchema {
  // Bounds how long a permitted enum value may be. This is what makes "an
  // offloaded string cannot equal any permitted value" a sound conclusion rather
  // than a guess: a string only leaves the DOM once it exceeds the parser's
  // inline threshold, which is far larger than this.
  static constexpr std::size_t kMaxEnumValueBytes = 64;

  FieldKind kind{FieldKind::AnyJson};

  // String: the permitted values; empty means unconstrained. Views point at
  // string literals, so they are valid for the process's lifetime.
  std::vector<absl::string_view> enum_values;

  // String only: whether this value may be left in the external buffer instead
  // of being materialized in the DOM.
  //
  // Mutually exclusive with enum_values, enforced by the builder: a value the
  // proxy has to compare against something must be a value the proxy can read.
  bool offloadable{false};
  // Meaningful only when offloadable.
  std::uint32_t stream_order{StreamOrder::Other};

  // Number/Int: inclusive bounds. Unset means unbounded.
  //
  // Not named min/max: those are macros in windows.h, which Envoy builds
  // against.
  std::optional<double> min_value;
  std::optional<double> max_value;

  // Array: the minimum element count.
  std::size_t min_items{0};
  // Array: what every element must satisfy. Null leaves elements unconstrained.
  const FieldSchema* element{nullptr};

  // Object: the declared fields. A key that is not here passes untouched -- this
  // is a proxy, not a validating gateway, so an undeclared field is forwarded as
  // it stands.
  ObjectFields fields;
  // Object: how many entries of `fields` are required. Derived by the builder so
  // the walk can detect a missing required field with one counter instead of a
  // second pass over `fields`.
  std::size_t required_field_count{0};

  // OneOf: the forms the value may take. Semantically anyOf -- the first
  // alternative that validates wins, and a value satisfying two of them is
  // accepted. Strict exactly-one would reject values these deliberately loose
  // alternatives both happen to admit.
  std::vector<const FieldSchema*> alternatives;
};

// Tag making a required field read as one at the declaration site.
struct RequiredTag {};
constexpr RequiredTag Required{};

// One entry of an object declaration.
//
// The two constructors are what let an optional field be written
// `{"temperature", b.number(0, 2)}` and a required one
// `{"model", Required, b.str()}`, with no `Optional` noise on the many fields
// that are optional.
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
// LIFETIME: every node is individually heap-allocated and never moved, so a
// pointer handed back stays valid for as long as the builder lives -- including
// across a move of the builder itself, since that moves the unique_ptrs and not
// the nodes they point at. This is also why ObjectFields storing pointers rather
// than nodes matters: absl::flat_hash_map does not keep value references stable
// across insertion, but it has no reason to move what it does not own.
//
// A builder is only ever a member of a process-lifetime object
// (tree_payload_schema.h), so in practice every pointer it returns is immortal.
class SchemaBuilder {
public:
  const FieldSchema* str();
  const FieldSchema* str(std::vector<absl::string_view> enum_values);
  // A string that may stay in the external buffer. Cannot carry an enum.
  const FieldSchema* offloadableStr(std::uint32_t stream_order);
  const FieldSchema* number(std::optional<double> min_value = std::nullopt,
                            std::optional<double> max_value = std::nullopt);
  const FieldSchema* integer(std::optional<double> min_value = std::nullopt,
                             std::optional<double> max_value = std::nullopt);
  const FieldSchema* boolean();
  const FieldSchema* anyJson();
  const FieldSchema* object(std::initializer_list<FieldDecl> fields);
  // An object whose contents are unconstrained: must be an object, nothing more.
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
