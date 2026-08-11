#pragma once

#include "source/extensions/filters/http/ai_protocol_manager/schema/field_schema.h"

#include "absl/status/status.h"
#include "nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// Validates a parsed payload against a schema tree.
//
// On a violation, InvalidArgument whose message is "<path>: <reason>", e.g.
// "messages[2].role: expected a string" or "model: required field is missing".
//
// The message never contains any part of the payload: every path segment comes
// from the schema declaration or is an array index, and every reason is a literal,
// optionally with the schema's own numeric bounds. It reaches the client and the
// access log, and prompt content must reach neither. A test pins this.
//
// Only the first violation is reported -- a proxy needs a reason to reject, not an
// audit report.
//
// An oversized string is not in the DOM: it is a binary node holding an
// ExternalRef (json_with_ext_buf.h), so FieldKind::String accepts
// `is_string() || isExternalRef()`. No schema constrains a string's contents, so
// nothing is skipped by not reading the buffer.
// TODO(penguingao): a future string constraint (length, pattern) could not be
// checked on an offloaded value; it would need the reference resolved against the
// external buffer during validation.
//
// TODO(penguingao): validate as the Wuffs parser goes rather than walking the
// finished DOM. The parser already reports the path and depth of each value, so a
// violation could fail the request at the offending byte -- as a parse error
// already does -- instead of after the whole upload, and would not need the DOM to
// be complete first.
absl::Status validate(const nlohmann::json& payload, const FieldSchema& schema);

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
