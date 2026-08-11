#include "source/extensions/filters/http/ai_protocol_manager/schema/field_schema.h"

#include <utility>

#include "source/common/common/assert.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// The assertions here guard a hand-written table against typos. They run once per
// process, when the registry first builds a schema.

const FieldSchema* SchemaBuilder::intern(FieldSchema&& node) {
  nodes_.push_back(std::make_unique<FieldSchema>(std::move(node)));
  return nodes_.back().get();
}

const FieldSchema* SchemaBuilder::str() {
  FieldSchema node;
  node.kind = FieldKind::String;
  return intern(std::move(node));
}

const FieldSchema* SchemaBuilder::offloadableStr() {
  FieldSchema node;
  node.kind = FieldKind::String;
  node.offloadable = true;
  return intern(std::move(node));
}

const FieldSchema* SchemaBuilder::number(std::optional<double> min_value,
                                         std::optional<double> max_value) {
  RELEASE_ASSERT(!min_value.has_value() || !max_value.has_value() || *min_value <= *max_value,
                 "schema: number bounds are inverted");
  FieldSchema node;
  node.kind = FieldKind::Number;
  node.min_value = min_value;
  node.max_value = max_value;
  return intern(std::move(node));
}

const FieldSchema* SchemaBuilder::integer(std::optional<double> min_value,
                                          std::optional<double> max_value) {
  RELEASE_ASSERT(!min_value.has_value() || !max_value.has_value() || *min_value <= *max_value,
                 "schema: integer bounds are inverted");
  FieldSchema node;
  node.kind = FieldKind::Int;
  node.min_value = min_value;
  node.max_value = max_value;
  return intern(std::move(node));
}

const FieldSchema* SchemaBuilder::boolean() {
  FieldSchema node;
  node.kind = FieldKind::Bool;
  return intern(std::move(node));
}

const FieldSchema* SchemaBuilder::anyJson() {
  FieldSchema node;
  node.kind = FieldKind::AnyJson;
  return intern(std::move(node));
}

const FieldSchema* SchemaBuilder::object(std::initializer_list<FieldDecl> fields) {
  FieldSchema node;
  node.kind = FieldKind::Object;
  for (const FieldDecl& field : fields) {
    RELEASE_ASSERT(!field.name.empty(), "schema: an object field needs a name");
    RELEASE_ASSERT(field.schema != nullptr, "schema: object field has no schema");
    RELEASE_ASSERT(
        node.fields.emplace(field.name, ObjectField{field.schema, field.required}).second,
        "schema: duplicate object field name");
    if (field.required) {
      ++node.required_field_count;
    }
  }
  return intern(std::move(node));
}

const FieldSchema* SchemaBuilder::array(const FieldSchema* element, std::size_t min_items) {
  // A null element is legal: the elements are unconstrained.
  FieldSchema node;
  node.kind = FieldKind::Array;
  node.element = element;
  node.min_items = min_items;
  return intern(std::move(node));
}

const FieldSchema* SchemaBuilder::oneOf(std::initializer_list<const FieldSchema*> alternatives) {
  // One alternative is a declaration mistake, not a shape.
  RELEASE_ASSERT(alternatives.size() >= 2, "schema: oneOf needs at least two alternatives");
  FieldSchema node;
  node.kind = FieldKind::OneOf;
  for (const FieldSchema* alternative : alternatives) {
    RELEASE_ASSERT(alternative != nullptr, "schema: oneOf alternative is null");
    node.alternatives.push_back(alternative);
  }
  return intern(std::move(node));
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
