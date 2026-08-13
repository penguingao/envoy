#include "source/extensions/filters/http/ai_protocol_manager/json_emitter.h"

#include <cmath>
#include <utility>

#include "source/common/buffer/buffer_util.h"
#include "source/common/json/constants.h"
#include "source/common/json/json_sanitizer.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

namespace {

// Adapts a std::string to the `add(absl::string_view)` sink that
// Buffer::Util::serializeDouble writes through.
struct StringSink {
  void add(absl::string_view text) { out.append(text.data(), text.size()); }
  std::string& out;
};

} // namespace

JsonEmitter::JsonEmitter(const nlohmann::json& root, uint32_t flush_bytes)
    : flush_bytes_(flush_bytes) {
  stack_.push_back(Step{Step::Kind::Value, &root, {}, {}});
}

JsonEmitter::State JsonEmitter::next() {
  out_.clear();

  // The opening quote was handed back with the previous run of text; the range
  // itself is what this call owes the caller.
  if (range_pending_) {
    range_pending_ = false;
    return State::Range;
  }
  if (finished_) {
    return State::Done;
  }

  while (!stack_.empty()) {
    step();
    if (!status_.ok()) {
      finished_ = true;
      return State::Done;
    }
    // Hand back everything up to and including the opening quote, so the
    // spliced bytes land immediately after it.
    if (range_pending_) {
      return State::Text;
    }
    if (out_.size() >= flush_bytes_) {
      return State::Text;
    }
  }

  finished_ = true;
  return out_.empty() ? State::Done : State::Text;
}

void JsonEmitter::step() {
  const Step current = stack_.back();
  stack_.pop_back();

  switch (current.kind) {
  case Step::Kind::Literal:
    out_.append(current.literal.data(), current.literal.size());
    return;

  case Step::Kind::Value:
    emitValue(*current.node);
    return;

  case Step::Kind::ObjectMember: {
    if (current.it == current.node->end()) {
      return;
    }
    if (current.it != current.node->begin()) {
      out_.append(Json::Constants::Comma);
    }
    emitString(current.it.key());
    out_.append(Json::Constants::Colon);

    // Queue the remaining members behind this one's value, so the value is
    // serialized first and the iteration resumes after it.
    nlohmann::json::const_iterator rest = current.it;
    ++rest;
    stack_.push_back(Step{Step::Kind::ObjectMember, current.node, rest, {}});
    stack_.push_back(Step{Step::Kind::Value, &current.it.value(), {}, {}});
    return;
  }

  case Step::Kind::ArrayElement: {
    if (current.it == current.node->end()) {
      return;
    }
    if (current.it != current.node->begin()) {
      out_.append(Json::Constants::Comma);
    }
    nlohmann::json::const_iterator rest = current.it;
    ++rest;
    stack_.push_back(Step{Step::Kind::ArrayElement, current.node, rest, {}});
    stack_.push_back(Step{Step::Kind::Value, &(*current.it), {}, {}});
    return;
  }
  }
}

void JsonEmitter::emitValue(const nlohmann::json& node) {
  // Checked before the type switch: an offload reference is carried as a binary
  // node, so it would otherwise be mistaken for one.
  if (JsonWithExtBuf::isExternalRef(node)) {
    const absl::StatusOr<JsonWithExtBuf::ExternalRef> ref = JsonWithExtBuf::externalRef(node);
    if (!ref.ok()) {
      setError(ref.status());
      return;
    }
    out_.append(Json::Constants::DoubleQuote);
    range_ = *ref;
    range_pending_ = true;
    // The spliced bytes exclude the quotes, so this closes the string once they
    // have been written.
    stack_.push_back(Step{Step::Kind::Literal, nullptr, {}, Json::Constants::DoubleQuote});
    return;
  }

  switch (node.type()) {
  case nlohmann::json::value_t::object:
    out_.append(Json::Constants::MapBegin);
    stack_.push_back(Step{Step::Kind::Literal, nullptr, {}, Json::Constants::MapEnd});
    stack_.push_back(Step{Step::Kind::ObjectMember, &node, node.begin(), {}});
    return;

  case nlohmann::json::value_t::array:
    out_.append(Json::Constants::ArrayBegin);
    stack_.push_back(Step{Step::Kind::Literal, nullptr, {}, Json::Constants::ArrayEnd});
    stack_.push_back(Step{Step::Kind::ArrayElement, &node, node.begin(), {}});
    return;

  case nlohmann::json::value_t::string:
    emitString(node.get_ref<const std::string&>());
    return;

  case nlohmann::json::value_t::boolean:
    out_.append(node.get<bool>() ? Json::Constants::True : Json::Constants::False);
    return;

  case nlohmann::json::value_t::number_integer:
  case nlohmann::json::value_t::number_unsigned:
  case nlohmann::json::value_t::number_float:
    emitNumber(node);
    return;

  case nlohmann::json::value_t::null:
    out_.append(Json::Constants::Null);
    return;

  case nlohmann::json::value_t::binary:
    // A binary node that is not an offload reference has no JSON rendering, and
    // the parser never produces one.
    setError(absl::InternalError("ai json emit: unexpected binary node"));
    return;

  case nlohmann::json::value_t::discarded:
  default:
    setError(absl::InternalError("ai json emit: unserializable node"));
    return;
  }
}

void JsonEmitter::emitString(absl::string_view value) {
  out_.append(Json::Constants::DoubleQuote);
  out_.append(Json::sanitize(sanitize_buffer_, value));
  out_.append(Json::Constants::DoubleQuote);
}

void JsonEmitter::emitNumber(const nlohmann::json& node) {
  if (node.is_number_integer() && !node.is_number_unsigned()) {
    absl::StrAppend(&out_, node.get<std::int64_t>());
    return;
  }
  if (node.is_number_unsigned()) {
    absl::StrAppend(&out_, node.get<std::uint64_t>());
    return;
  }
  const double value = node.get<double>();
  // JSON has no literal for these; null is what a conforming serializer emits.
  if (std::isnan(value) || std::isinf(value)) {
    out_.append(Json::Constants::Null);
    return;
  }
  StringSink sink{out_};
  Buffer::Util::serializeDouble(value, sink);
}

void JsonEmitter::setError(absl::Status status) {
  if (status_.ok()) {
    status_ = std::move(status);
  }
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
