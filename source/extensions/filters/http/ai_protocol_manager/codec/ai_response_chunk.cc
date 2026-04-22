#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_response_chunk.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

AiResponseChunk AiResponseChunk::makeStarted(ChunkStarted&& s, size_t item_index) {
  AiResponseChunk c;
  c.kind_ = AiChunkKind::Started;
  c.item_index_ = item_index;
  c.started_ = std::make_unique<ChunkStarted>(std::move(s));
  return c;
}

AiResponseChunk AiResponseChunk::makeItemAdded(ChunkItemAdded&& i, size_t item_index) {
  AiResponseChunk c;
  c.kind_ = AiChunkKind::ItemAdded;
  c.item_index_ = item_index;
  c.item_added_ = std::make_unique<ChunkItemAdded>(std::move(i));
  return c;
}

AiResponseChunk AiResponseChunk::makeContentDelta(ChunkContentDelta&& d, size_t item_index) {
  AiResponseChunk c;
  c.kind_ = AiChunkKind::ContentDelta;
  c.item_index_ = item_index;
  c.content_delta_ = std::make_unique<ChunkContentDelta>(std::move(d));
  return c;
}

AiResponseChunk AiResponseChunk::makeReasoningDelta(ChunkReasoningDelta&& d, size_t item_index) {
  AiResponseChunk c;
  c.kind_ = AiChunkKind::ReasoningDelta;
  c.item_index_ = item_index;
  c.reasoning_delta_ = std::make_unique<ChunkReasoningDelta>(std::move(d));
  return c;
}

AiResponseChunk AiResponseChunk::makeToolCallDelta(ChunkToolCallDelta&& d, size_t item_index) {
  AiResponseChunk c;
  c.kind_ = AiChunkKind::ToolCallDelta;
  c.item_index_ = item_index;
  c.tool_call_delta_ = std::make_unique<ChunkToolCallDelta>(std::move(d));
  return c;
}

AiResponseChunk AiResponseChunk::makeItemDone(ChunkItemDone&& i, size_t item_index) {
  AiResponseChunk c;
  c.kind_ = AiChunkKind::ItemDone;
  c.item_index_ = item_index;
  c.item_done_ = std::make_unique<ChunkItemDone>(std::move(i));
  return c;
}

AiResponseChunk AiResponseChunk::makeCompleted(ChunkCompleted&& co, size_t item_index) {
  AiResponseChunk c;
  c.kind_ = AiChunkKind::Completed;
  c.item_index_ = item_index;
  c.completed_ = std::make_unique<ChunkCompleted>(std::move(co));
  return c;
}

AiResponseChunk AiResponseChunk::makeErrorEvent(ChunkErrorEvent&& e, size_t item_index) {
  AiResponseChunk c;
  c.kind_ = AiChunkKind::ErrorEvent;
  c.item_index_ = item_index;
  c.error_event_ = std::make_unique<ChunkErrorEvent>(std::move(e));
  return c;
}

AiResponseChunk AiResponseChunk::makeFinal(ChunkFinalBody&& f) {
  AiResponseChunk c;
  c.kind_ = AiChunkKind::Final;
  c.item_index_ = 0;
  c.final_body_ = std::make_unique<ChunkFinalBody>(std::move(f));
  return c;
}

AiResponseChunk AiResponseChunk::makeRaw(ChunkRaw&& r, size_t item_index) {
  AiResponseChunk c;
  c.kind_ = AiChunkKind::Raw;
  c.item_index_ = item_index;
  c.raw_ = std::make_unique<ChunkRaw>(std::move(r));
  return c;
}

ChunkStarted* AiResponseChunk::asStarted() { return started_.get(); }
ChunkItemAdded* AiResponseChunk::asItemAdded() { return item_added_.get(); }
ChunkContentDelta* AiResponseChunk::asContentDelta() { return content_delta_.get(); }
ChunkReasoningDelta* AiResponseChunk::asReasoningDelta() { return reasoning_delta_.get(); }
ChunkToolCallDelta* AiResponseChunk::asToolCallDelta() { return tool_call_delta_.get(); }
ChunkItemDone* AiResponseChunk::asItemDone() { return item_done_.get(); }
ChunkCompleted* AiResponseChunk::asCompleted() { return completed_.get(); }
ChunkErrorEvent* AiResponseChunk::asErrorEvent() { return error_event_.get(); }
ChunkFinalBody* AiResponseChunk::asFinal() { return final_body_.get(); }
ChunkRaw* AiResponseChunk::asRaw() { return raw_.get(); }

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
