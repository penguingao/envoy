#pragma once

#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_response_decoder.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

// Gemini generateContent → OpenAI chat.completion response decoder.
//
// Symmetric to GeminiEncoder on the request side. Implements the subset of
// OPENAI_VERTEX_SPEC.md §4 that handles non-streaming bodies:
//   - candidates[] → choices[]
//   - content.parts[] (text + functionCall) → message.content + message.tool_calls
//   - finishReason → finish_reason mapping (§4.3)
//   - usageMetadata → usage (§4.2; thoughts and prompt-cache details
//     fold into the OpenAI-compat `usage` block)
//
// Streaming SSE (§6) lands as a separate codepath in a later phase; this
// class will gain onChunk / onEnd methods then. For now, attempting to
// decode an SSE body produces an InvalidArgument since the bytes are not a
// single JSON document.
//
// What is intentionally not (yet) translated:
//   - safetyRatings, groundingMetadata, logprobs (passed through as
//     OpenAI extension fields once we have a place for them)
//   - ThoughtSignature attachment to first tool call
//   - audio / multimodal output parts
class GeminiResponseDecoder : public AiResponseDecoder {
public:
  absl::Status decodeFullBody(absl::string_view upstream_body, AiResponse& ai_response,
                              Buffer::Instance& out_body) override;
};

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
