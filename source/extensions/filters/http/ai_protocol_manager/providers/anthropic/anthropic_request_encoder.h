#pragma once

#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/request_encoder.h"

#include "absl/types/optional.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Providers {

// AnthropicRequestEncoder translates an OpenAI-format InferencePayload into
// an Anthropic Messages API HTTP request.
//
// Mapping summary (OpenAI Chat Completions → Anthropic Messages API):
//
//   Field mapping:
//     model                     → model          (verbatim)
//     messages[role=system]     → system         (extracted, concatenated)
//     messages[role=user]       → messages[]     (content blocks converted)
//     messages[role=assistant]  → messages[]     (tool_calls → tool_use blocks)
//     messages[role=tool]       → messages[]     (merged into user tool_result blocks)
//     max_tokens                → max_tokens     (required; defaults to 4096 if absent)
//     temperature               → temperature
//     top_p                     → top_p
//     stop (string|array)       → stop_sequences (array)
//     stream                    → stream
//     tools[].function          → tools[]        (parameters → input_schema)
//     tool_choice (string|obj)  → tool_choice    (converted to Anthropic object form)
//
//   Dropped fields (not supported by Anthropic):
//     n, seed, logprobs, logit_bias, frequency_penalty, presence_penalty
//
//   Path rewrite:
//     /v1/chat/completions → /v1/messages
//
// Supported invocations:
//   ChatCompletion  → full translation
//   Completion      → prompt field wrapped as a user message
//
// All other invocations (Embeddings, Responses* resource ops) return nullopt;
// InferenceDispatch falls back to OpenAI round-trip encoding in that case.
class AnthropicRequestEncoder {
public:
  // Translates request into an Anthropic Messages API request.
  // Returns nullopt for unsupported invocation types or on parse failure of
  // a required field.
  static absl::optional<Codec::RestHttpRequest> encode(const Codec::AiRequest& request);
};

} // namespace Providers
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
