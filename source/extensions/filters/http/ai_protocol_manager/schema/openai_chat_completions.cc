#include "source/extensions/filters/http/ai_protocol_manager/schema/openai_chat_completions.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// WHAT IS CONSTRAINED
//
// Types, required fields, array bounds, and the numeric ranges OpenAI documents as
// hard. Not values: the sets churn faster than an Envoy release, and rejecting a
// request the upstream would have accepted is worse than forwarding one it rejects
// itself, because only the first is a failure the proxy invented. Undeclared
// fields passing through is the same principle -- a field OpenAI ships next month
// needs no change here.
//
// Not expressible in a per-field tree, and left to the upstream: stream_options
// requires stream: true; content is required for a user message but nullable on a
// tool-calling assistant one; tool_call_id is required only for role: tool;
// logit_bias values are bounded -100..100.
//
// Fields marked offloadable are only the free text; everything else is declared to
// stay inline so a later filter can read it. Nothing acts on that yet -- see
// FieldSchema::offloadable.

const FieldSchema* buildOpenAiChatCompletionsRequestSchema(SchemaBuilder& b) {
  // Bottom-up: a parent needs its children's pointers. Shared subtrees are
  // declared once and referenced from every parent.

  // One element of the multi-part content array. Reached from a message's content
  // and from prediction.content.
  const FieldSchema* content_part = b.object({
      {"type", Required, b.str()},
      // The prompt: the field the external-buffer design exists for.
      {"text", b.offloadableStr()},
      {"refusal", b.str()},
      {"image_url", b.object({
                        {"url", Required, b.offloadableStr()}, // Data URIs are large.
                        {"detail", b.str()},
                    })},
      // Base64 blobs and file handles: shapes churn and the bytes are opaque to
      // routing, so only the discriminator above is held.
      {"input_audio", b.anyObject()},
      {"file", b.anyObject()},
  });

  // A string or the multi-part array. The null form of a tool-calling assistant
  // message is covered by the validator's rule for an optional field.
  const FieldSchema* content = b.oneOf({b.offloadableStr(), b.array(content_part)});

  const FieldSchema* tool_call = b.object({
      {"id", Required, b.str()},
      {"type", b.str()},
      {"function", b.object({
                       {"name", b.str()},
                       // Caller-authored, often large, and not reliably valid
                       // JSON -- a model can emit a truncated argument blob.
                       {"arguments", b.offloadableStr()},
                   })},
      {"custom", b.anyObject()},
  });

  const FieldSchema* message = b.object({
      {"role", Required, b.str()},
      // Optional, not required: absent on an assistant turn carrying only
      // tool_calls. Which roles require it is a cross-field rule.
      {"content", content},
      {"name", b.str()},
      {"tool_call_id", b.str()},
      {"tool_calls", b.array(tool_call)},
      {"refusal", b.str()},
      {"audio", b.anyObject()},
      {"function_call", b.anyObject()}, // Deprecated in favor of tool_calls.
  });

  const FieldSchema* tool = b.object({
      {"type", Required, b.str()},
      {"function", b.object({
                       {"name", Required, b.str()},
                       {"description", b.offloadableStr()},
                       {"strict", b.boolean()},
                       // Caller-supplied JSON Schema, carried through untouched.
                       // AnyJson rather than anyObject so a client sending some
                       // other shape is the upstream's problem, not a 400 here.
                       {"parameters", b.anyJson()},
                   })},
      {"custom", b.anyObject()},
  });

  return b.object({
      {"model", Required, b.str()},
      {"messages", Required, b.array(message, /*min_items=*/1)},

      // Documented, inclusive, unchanged since launch.
      {"temperature", b.number(0.0, 2.0)},
      {"top_p", b.number(0.0, 1.0)},
      {"presence_penalty", b.number(-2.0, 2.0)},
      {"frequency_penalty", b.number(-2.0, 2.0)},
      {"top_logprobs", b.integer(0, 20)},
      // Lower bound only: upper caps are model-specific and move.
      {"n", b.integer(/*min_value=*/1)},
      {"max_tokens", b.integer(/*min_value=*/1)}, // Deprecated, still widely sent.
      {"max_completion_tokens", b.integer(/*min_value=*/1)},
      {"seed", b.integer()},
      {"logprobs", b.boolean()},

      // Envoy acts on this one: it decides whether the response is SSE.
      {"stream", b.boolean()},
      {"stream_options", b.object({{"include_usage", b.boolean()}})},

      {"stop", b.oneOf({b.str(), b.array(b.str())})},
      {"tools", b.array(tool)},
      {"tool_choice", b.oneOf({b.str(), b.object({
                                            {"type", Required, b.str()},
                                            {"function", b.object({{"name", Required, b.str()}})},
                                            {"custom", b.anyObject()},
                                        })})},
      {"parallel_tool_calls", b.boolean()},
      {"response_format", b.object({
                              {"type", Required, b.str()},
                              {"json_schema", b.object({
                                                  {"name", Required, b.str()},
                                                  {"description", b.str()},
                                                  {"strict", b.boolean()},
                                                  {"schema", b.anyJson()},
                                              })},
                          })},
      {"prediction", b.object({
                         {"type", Required, b.str()},
                         {"content", content},
                     })},

      {"store", b.boolean()},
      {"user", b.str()},
      {"modalities", b.array(b.str())},
      {"audio", b.object({{"voice", b.str()}, {"format", b.str()}})},
      // Dynamic key -> value maps; this shape has no schema for an object's
      // values, so contents are unconstrained.
      {"logit_bias", b.anyObject()},
      {"metadata", b.anyObject()},
      {"service_tier", b.str()},
      {"reasoning_effort", b.str()},
  });
}

const FieldSchema* buildOpenAiChatCompletionsResponseSchema(SchemaBuilder& b) {
  return b.anyObject();
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
