#include "source/extensions/filters/http/ai_protocol_manager/schema/schema_validator.h"

#include <cmath>
#include <cstddef>
#include <string>

#include "envoy/common/exception.h"

#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

// Appends one segment for the duration of a scope, so the walk carries one string
// and only the failing path is ever read.
class PathScope {
public:
  PathScope(std::string& path, absl::string_view field) : path_(path), saved_(path.size()) {
    if (!path_.empty()) {
      path_.push_back('.');
    }
    path_.append(field.data(), field.size());
  }
  PathScope(std::string& path, std::size_t index) : path_(path), saved_(path.size()) {
    absl::StrAppend(&path_, "[", index, "]");
  }
  ~PathScope() { path_.resize(saved_); }

  PathScope(const PathScope&) = delete;
  PathScope& operator=(const PathScope&) = delete;

private:
  std::string& path_;
  const std::size_t saved_;
};

class Validator {
public:
  absl::Status validateNode(const nlohmann::json& value, const FieldSchema& schema);

private:
  // Every reason is a literal, optionally with the schema's own bounds. No caller
  // may pass anything derived from the payload.
  absl::Status violation(absl::string_view reason) const {
    return absl::InvalidArgumentError(absl::StrCat(
        path_.empty() ? absl::string_view("payload") : absl::string_view(path_), ": ", reason));
  }

  absl::Status validateNumber(const nlohmann::json& value, const FieldSchema& schema) const;
  absl::Status validateObject(const nlohmann::json& value, const FieldSchema& schema);
  absl::Status validateArray(const nlohmann::json& value, const FieldSchema& schema);
  absl::Status validateOneOf(const nlohmann::json& value, const FieldSchema& schema);

  std::string path_;
};

absl::Status Validator::validateNumber(const nlohmann::json& value,
                                       const FieldSchema& schema) const {
  const bool is_int = schema.kind == FieldKind::Int;
  if (!value.is_number()) {
    return violation(is_int ? "expected an integer" : "expected a number");
  }
  const double number = value.get<double>();
  // 1024.0 where an integer belongs is not worth rejecting when the upstream would
  // accept it; 1.5 is.
  if (is_int && std::trunc(number) != number) {
    return violation("expected an integer");
  }
  if (schema.min_value.has_value() && number < *schema.min_value) {
    return violation(absl::StrCat("value must be at least ", *schema.min_value));
  }
  if (schema.max_value.has_value() && number > *schema.max_value) {
    return violation(absl::StrCat("value must be at most ", *schema.max_value));
  }
  return absl::OkStatus();
}

absl::Status Validator::validateObject(const nlohmann::json& value, const FieldSchema& schema) {
  if (!value.is_object()) {
    return violation("expected an object");
  }

  // Walking the payload, not the schema: unknown fields pass, so the schema is a
  // lookup table. Probing it costs one hash of a string already in the DOM,
  // whereas walking the schema would need a std::string per declared name to look
  // up in nlohmann's std::map-backed object.
  std::size_t required_seen = 0;
  for (const auto& entry : value.items()) {
    const auto it = schema.fields.find(entry.key());
    if (it == schema.fields.end()) {
      continue; // Undeclared: forwarded untouched.
    }
    const ObjectField& field = it->second;
    // The segment is the schema's key, not the payload's, so the path is provably
    // schema text.
    PathScope scope(path_, it->first);
    if (field.required) {
      ++required_seen;
      if (entry.value().is_null()) {
        return violation("must not be null");
      }
    } else if (entry.value().is_null()) {
      // An explicit null on an optional field means "unset" across the OpenAI
      // surface ("stop": null, a tool-calling assistant's "content": null), so it
      // satisfies the field without reaching its kind check.
      //
      // Applied here, at the field edge where `required` is known, and not in
      // validateNode(): an array element is not an optional field, so
      // "messages": [null] is still a violation.
      continue;
    }
    RETURN_IF_NOT_OK(validateNode(entry.value(), *field.schema));
  }

  if (required_seen != schema.required_field_count) {
    // Error path only, so naming the absent field can afford a second pass.
    for (const auto& [name, field] : schema.fields) {
      if (field.required && value.find(std::string(name)) == value.end()) {
        PathScope scope(path_, name);
        return violation("required field is missing");
      }
    }
  }
  return absl::OkStatus();
}

absl::Status Validator::validateArray(const nlohmann::json& value, const FieldSchema& schema) {
  if (!value.is_array()) {
    return violation("expected an array");
  }
  if (value.size() < schema.min_items) {
    return violation(schema.min_items == 1
                         ? std::string("must not be empty")
                         : absl::StrCat("must have at least ", schema.min_items, " elements"));
  }
  if (schema.element == nullptr) {
    return absl::OkStatus();
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    PathScope scope(path_, i);
    RETURN_IF_NOT_OK(validateNode(value[i], *schema.element));
  }
  return absl::OkStatus();
}

absl::Status Validator::validateOneOf(const nlohmann::json& value, const FieldSchema& schema) {
  for (const FieldSchema* alternative : schema.alternatives) {
    if (validateNode(value, *alternative).ok()) {
      return absl::OkStatus();
    }
  }
  // The alternatives' own reasons are dropped: a wall of them is noise, and the
  // client's fix is to read the schema.
  return violation("value does not match any permitted form");
}

absl::Status Validator::validateNode(const nlohmann::json& value, const FieldSchema& schema) {
  // Recursion is bounded: the parser's cursor caps nesting well before this.
  switch (schema.kind) {
  case FieldKind::String:
    // An offloaded string is a binary node, not a string node; see the header.
    return value.is_string() || JsonWithExtBuf::isExternalRef(value)
               ? absl::OkStatus()
               : violation("expected a string");
  case FieldKind::Number:
  case FieldKind::Int:
    return validateNumber(value, schema);
  case FieldKind::Bool:
    return value.is_boolean() ? absl::OkStatus() : violation("expected a boolean");
  case FieldKind::Object:
    return validateObject(value, schema);
  case FieldKind::Array:
    return validateArray(value, schema);
  case FieldKind::AnyJson:
    return absl::OkStatus();
  case FieldKind::OneOf:
    return validateOneOf(value, schema);
  }
  return absl::OkStatus();
}

} // namespace

absl::Status validate(const nlohmann::json& payload, const FieldSchema& schema) {
  Validator validator;
  return validator.validateNode(payload, schema);
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
