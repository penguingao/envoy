#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// Serializes a JsonWithExtBuf DOM back to JSON text, stopping wherever an
// offloaded value has to be spliced in from somewhere else.
//
// A payload that no one modified is forwarded by replaying its original bytes,
// so this exists for the payload that was modified. Its problem is that the DOM
// is not self-contained: an offloaded value is an ExternalRef standing in for
// bytes that live in the external buffer, and those may be far larger than the
// per-stream memory the proxy is willing to spend. Materializing them to
// serialize would give back exactly the footprint offloading bought.
//
// So this does not return a document. It is a pull-driven state machine that
// yields either a run of text it produced or a byte range the caller must
// splice in verbatim, and it can be stopped at either. Memory is bounded by the
// flush threshold regardless of payload size, and an offloaded value is never
// held.
//
//   JsonEmitter emitter(request.json());
//   while (true) {
//     switch (emitter.next()) {
//     case JsonEmitter::State::Text:  write(emitter.text()); break;
//     case JsonEmitter::State::Range: spliceFromBuffer(emitter.range()); break;
//     case JsonEmitter::State::Done:  return emitter.status();
//     }
//   }
//
// The bytes an ExternalRef names are raw JSON content, already escaped and
// without the enclosing quotes (json_with_ext_buf.h), which is exactly what
// belongs between the quotes this emits -- so a range is spliced verbatim and
// never re-escaped.
//
// Not nlohmann's dump(): it throws, it cannot be stopped partway, and it would
// render an ExternalRef node as a binary blob rather than the string it stands
// for.
//
// Object members come out in the DOM's order, which for nlohmann's std::map is
// lexicographic by key rather than the order they arrived in. JSON objects are
// unordered, so this is a well-formed rendering of the same document, but it is
// not byte-identical to the input -- another reason an unmodified payload is
// replayed rather than re-emitted.
class JsonEmitter {
public:
  // Text is accumulated until it reaches this size, then handed back. Bounds the
  // emitter's footprint; the value only trades syscalls against memory.
  static constexpr uint32_t DefaultFlushBytes = 16 * 1024;

  // What next() produced.
  enum class State {
    // text() holds the next run of output.
    Text,
    // range() must be spliced into the output before emitting continues.
    Range,
    // The document is complete, or emitting failed -- check status().
    Done,
  };

  // `root` must outlive the emitter; nothing is copied.
  explicit JsonEmitter(const nlohmann::json& root, uint32_t flush_bytes = DefaultFlushBytes);

  // Advances until a run of text is ready, a range must be spliced, or the
  // document ends.
  State next();

  // Valid after next() returned Text, until the following next().
  const std::string& text() const { return out_; }

  // Valid after next() returned Range.
  const JsonWithExtBuf::ExternalRef& range() const { return range_; }

  // Non-OK if the DOM held something that cannot be serialized, in which case
  // next() reports Done early and the output is incomplete.
  const absl::Status& status() const { return status_; }

private:
  // One unit of pending work. Ordering is what a recursive serializer would get
  // from the call stack; holding it explicitly is what lets emitting stop in the
  // middle of a value and resume later.
  struct Step {
    enum class Kind {
      // Serialize this node.
      Value,
      // Serialize this object's member at `it`, then the rest.
      ObjectMember,
      // Serialize this array's element at `it`, then the rest.
      ArrayElement,
      // Append fixed text (a closing bracket, or a string's closing quote).
      Literal,
    };

    Kind kind;
    const nlohmann::json* node{nullptr};
    nlohmann::json::const_iterator it{};
    absl::string_view literal;
  };

  // Runs one step, appending to out_ and pushing whatever it leaves to do.
  void step();

  // Serializes `node`: a scalar outright, a container by pushing its members.
  void emitValue(const nlohmann::json& node);

  // Appends `value` as a quoted, escaped JSON string.
  void emitString(absl::string_view value);

  void emitNumber(const nlohmann::json& node);

  void setError(absl::Status status);

  std::string out_;
  std::vector<Step> stack_;
  // Scratch for Json::sanitize(), reused across values.
  std::string sanitize_buffer_;
  const uint32_t flush_bytes_;

  // Set when a range has been reached. The opening quote is already in out_, so
  // the pending text is handed back first and the range on the call after.
  bool range_pending_{false};
  JsonWithExtBuf::ExternalRef range_;

  bool finished_{false};
  absl::Status status_;
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
