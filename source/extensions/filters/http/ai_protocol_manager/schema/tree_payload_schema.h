#pragma once

#include <string>

#include "source/extensions/filters/http/ai_protocol_manager/schema/field_schema.h"
#include "source/extensions/filters/http/ai_protocol_manager/schema/offload_plan.h"
#include "source/extensions/filters/http/ai_protocol_manager/schema/payload_schema.h"

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// Builds a schema tree into `builder` and returns its root. A schema is written
// as one of these (openai_chat_completions.h) rather than as a value, so the
// nodes are interned in the builder that will outlive them.
using SchemaBuildFn = const FieldSchema* (*)(SchemaBuilder&);

// A PayloadSchema backed by a FieldSchema tree.
//
// LIFETIME: `builder_` is declared before the members that point into it and is
// default-constructed in place -- never moved, never assigned -- so member
// initialization order makes root_ and plan_ correct by construction rather than
// by an argument about what a move does to interior pointers. That is also why the
// constructor takes a build function instead of a prebuilt tree: there is no way
// to hand it nodes owned by an arena it does not own.
class TreePayloadSchema : public PayloadSchema {
public:
  TreePayloadSchema(absl::string_view name, SchemaBuildFn build)
      : name_(name), root_(build(builder_)), plan_(*root_) {}

  // PayloadSchema
  absl::string_view name() const override { return name_; }
  absl::Status validate(const nlohmann::json& payload) const override;
  const OffloadPlan& offloadPlan() const override { return plan_; }

  // The tree itself, for tests and for a future transcoder that has to walk it.
  const FieldSchema& root() const { return *root_; }

private:
  const std::string name_;
  // Owns every node root_ and plan_ refer to. Must stay declared first.
  SchemaBuilder builder_;
  const FieldSchema* root_;
  const OffloadPlan plan_;
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
