#pragma once

#include "envoy/common/pure.h"

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// A schema a payload can be held to.
//
// This is what the filter programs against, so it never has to know how a schema
// is expressed. The tree-based implementation (tree_payload_schema.h) is the only
// one today; a provider whose shape is better described some other way -- a proto
// descriptor, say -- implements this rather than being forced into the tree.
class PayloadSchema {
public:
  virtual ~PayloadSchema() = default;

  // For logs and diagnostics, e.g. "openai_chat_completions".
  virtual absl::string_view name() const PURE;

  // OK if `payload` conforms. On a violation, InvalidArgument whose message is
  // "<path>: <reason>".
  //
  // An implementation MUST NOT put any part of the payload in that message: it
  // reaches the client and the access log, and prompt content must reach neither.
  // See schema_validator.h for how the tree implementation guarantees it.
  virtual absl::Status validate(const nlohmann::json& payload) const PURE;
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
