#pragma once

#include "source/extensions/filters/http/ai_protocol_manager/schema/field_schema.h"

#include "absl/status/status.h"
#include "nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// Validates a parsed payload against a schema tree by walking the two together.
//
// OK if `payload` conforms. On a violation, InvalidArgument whose message is
// "<path>: <reason>":
//
//   messages[2].role: value not permitted
//   temperature: value must be at most 2
//   model: required field is missing
//   payload: expected an object
//
// The message NEVER contains any part of the payload. Every path segment is
// either a field name taken from the schema declaration (a string literal) or an
// array index, and every reason is a literal, optionally carrying the schema's
// own numeric bounds. This message reaches the client and the access log, and
// prompt content must reach neither -- that is the property, and a test pins it.
//
// Only the first violation is reported: a proxy needs a reason to reject, not an
// audit report, and stopping early bounds the work an adversarial payload can
// cause.
//
// OFFLOADED STRINGS: a string whose decoded content exceeds the parser's inline
// threshold is not in the DOM at all -- it is a binary node carrying an
// ExternalRef (json_with_ext_buf.h). FieldKind::String therefore accepts
// `is_string() || JsonWithExtBuf::isExternalRef()`.
//
// An enum constraint on such a value is still decided, and decided against it:
// the value is by definition longer than the inline threshold, every permitted
// value is at most FieldSchema::kMaxEnumValueBytes, and the threshold is far
// larger -- so it cannot match any of them. Nothing has to be read out of the
// buffer to know that. In practice the case does not arise, because a schema
// marks only free text offloadable and never marks an enum-constrained field so.
//
// TODO(penguingao): a string constraint that is not an enum -- a length bound, a
// pattern -- cannot be decided this way. Resolving the reference against the
// external buffer during validation is the general fix; until then such a
// constraint must not be added.
absl::Status validate(const nlohmann::json& payload, const FieldSchema& schema);

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
