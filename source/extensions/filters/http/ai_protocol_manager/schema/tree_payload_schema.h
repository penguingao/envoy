#pragma once

#include <string>

#include "source/extensions/filters/http/ai_protocol_manager/schema/field_schema.h"
#include "source/extensions/filters/http/ai_protocol_manager/schema/payload_schema.h"

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// Builds a schema tree into `builder` and returns its root. A schema is written as
// one of these rather than as a value, so its nodes are interned in the builder
// that outlives them.
using SchemaBuildFn = const FieldSchema* (*)(SchemaBuilder&);

// A PayloadSchema backed by a FieldSchema tree.
//
// LIFETIME: `builder_` is declared before the members pointing into it and is
// default-constructed in place -- never moved -- so member initialization order
// makes root_ correct by construction. That is also why the constructor takes a
// build function rather than a prebuilt tree.
class TreePayloadSchema : public PayloadSchema {
public:
  TreePayloadSchema(absl::string_view name, SchemaBuildFn build)
      : name_(name), root_(build(builder_)) {}

  // PayloadSchema
  absl::string_view name() const override { return name_; }
  absl::Status validate(const nlohmann::json& payload) const override;

  // For tests, and for a future transcoder that has to walk the tree.
  const FieldSchema& root() const { return *root_; }

private:
  const std::string name_;
  // Owns every node root_ refers to. Must stay declared first.
  SchemaBuilder builder_;
  const FieldSchema* root_;
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
