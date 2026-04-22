#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_payload.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/inference_payload.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

// DESIGN.md §4.7 + ARCHITECTURE.md §2 — single-stream response chunks.
//
// Inference dispatch is one-in / one-out (ARCHITECTURE §2); there is no
// stream index here and no aggregation across backends. For a streaming
// response, one chunk per SSE event. For a non-streaming response, a
// single Final chunk carries the whole body.

enum class AiChunkKind {
  Started,         // response started: id, model, created-at.
  ItemAdded,       // a new output item / choice appeared.
  ContentDelta,    // text/content delta on an item.
  ReasoningDelta,  // reasoning block delta (o1, Responses reasoning).
  ToolCallDelta,   // tool-call name / arguments delta.
  ItemDone,        // an output item finished.
  Completed,       // response done: finish_reason, usage.
  ErrorEvent,      // upstream error event (mid-stream or terminal).
  Final,           // non-streaming: whole body as one chunk.
  Raw,             // protocol event the mapper did not model.
};

// Per-kind payloads. Each is populated only when AiResponseChunk::kind()
// matches; the typed accessor returns nullptr otherwise.

struct ChunkStarted {
  std::string id;          // response id echoed from backend
  std::string model;       // model actually used
  int64_t created_seconds{0};
};

struct ChunkItemAdded {
  std::string role;         // "assistant", "tool", ...
  std::string output_type;  // "message", "tool_call", "reasoning", ...
};

struct ChunkContentDelta {
  PayloadRef text;          // offloadable for large deltas
  std::string content_type; // "text/plain" when absent
};

struct ChunkReasoningDelta {
  PayloadRef text;
  // Optional signed thought signature (base64). Attached to the first
  // tool-call by the inference response mapper when tool-calls also exist
  // in the same assistant turn — see OPENAI_VERTEX_SPEC.md §4.1.
  std::string signature;
};

struct ChunkToolCallDelta {
  size_t tool_call_index{0};   // single-stream index within this response
  std::string name_delta;      // incremental
  PayloadRef arguments_delta;  // incremental JSON arguments
};

struct ChunkItemDone {
  // Some backends emit an explicit per-item terminator (Responses typed
  // events, e.g. `response.output_item.done`). Kept minimal for V0;
  // per-item usage / finish reasons land here when we need them.
};

struct ChunkCompleted {
  std::string finish_reason;
  InferenceResponseSummary::Usage usage;
};

struct ChunkErrorEvent {
  std::string code;     // provider error code (e.g. "INVALID_ARGUMENT")
  std::string message;  // human-readable message
  bool fatal{true};     // false for recoverable mid-stream warnings
};

struct ChunkFinalBody {
  PayloadRef body;
  std::string content_type;  // copied from upstream Content-Type
};

struct ChunkRaw {
  PayloadRef data;       // opaque event bytes
  std::string event_type; // "data", "error", SSE event name, ...
};

class AiResponseChunk {
public:
  AiResponseChunk() = default;

  static AiResponseChunk makeStarted(ChunkStarted&& s, size_t item_index = 0);
  static AiResponseChunk makeItemAdded(ChunkItemAdded&& i, size_t item_index);
  static AiResponseChunk makeContentDelta(ChunkContentDelta&& d, size_t item_index);
  static AiResponseChunk makeReasoningDelta(ChunkReasoningDelta&& d, size_t item_index);
  static AiResponseChunk makeToolCallDelta(ChunkToolCallDelta&& d, size_t item_index);
  static AiResponseChunk makeItemDone(ChunkItemDone&& i, size_t item_index);
  static AiResponseChunk makeCompleted(ChunkCompleted&& c, size_t item_index = 0);
  static AiResponseChunk makeErrorEvent(ChunkErrorEvent&& e, size_t item_index = 0);
  static AiResponseChunk makeFinal(ChunkFinalBody&& f);
  static AiResponseChunk makeRaw(ChunkRaw&& r, size_t item_index = 0);

  AiChunkKind kind() const { return kind_; }
  // Position within the response's output items (choices / tool calls / parts).
  // For Started / Completed / Final this is 0.
  size_t itemIndex() const { return item_index_; }

  // Mutation tracking — filters must call markDirty() when they change a
  // chunk's bytes. Clean chunks pass through by reference without
  // re-serialization. Started / Completed / ItemAdded / ItemDone are
  // scalar-only and do not participate in byte-level dirty tracking; filters
  // that want to rewrite them should do so at the response_encoder level.
  bool dirty() const { return dirty_; }
  void markDirty() { dirty_ = true; }

  // Typed accessors. Exactly one is non-null based on kind().
  ChunkStarted* asStarted();
  ChunkItemAdded* asItemAdded();
  ChunkContentDelta* asContentDelta();
  ChunkReasoningDelta* asReasoningDelta();
  ChunkToolCallDelta* asToolCallDelta();
  ChunkItemDone* asItemDone();
  ChunkCompleted* asCompleted();
  ChunkErrorEvent* asErrorEvent();
  ChunkFinalBody* asFinal();
  ChunkRaw* asRaw();

private:
  AiChunkKind kind_{AiChunkKind::Raw};
  size_t item_index_{0};
  bool dirty_{false};

  // Exactly one populated per kind_. Kept as separate optionals rather than a
  // variant so unit tests can move individual structs around without
  // re-sizing the whole chunk. Size is bounded by a single PayloadRef +
  // strings.
  std::unique_ptr<ChunkStarted> started_;
  std::unique_ptr<ChunkItemAdded> item_added_;
  std::unique_ptr<ChunkContentDelta> content_delta_;
  std::unique_ptr<ChunkReasoningDelta> reasoning_delta_;
  std::unique_ptr<ChunkToolCallDelta> tool_call_delta_;
  std::unique_ptr<ChunkItemDone> item_done_;
  std::unique_ptr<ChunkCompleted> completed_;
  std::unique_ptr<ChunkErrorEvent> error_event_;
  std::unique_ptr<ChunkFinalBody> final_body_;
  std::unique_ptr<ChunkRaw> raw_;
};

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
