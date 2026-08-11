#include "source/extensions/filters/http/ai_protocol_manager/schema/openai_chat_completions.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// WHAT IS CONSTRAINED, AND WHY NOT MORE
//
// Constrained: what is stable across providers and what Envoy or a policy filter
// acts on -- `model`, `messages` and `role`, `stream`, the sampling bounds OpenAI
// documents as hard ranges, and the discriminators a multi-form field is read
// through (`content[].type`, `response_format.type`, `tool_choice`'s string form).
//
// Left typed but unconstrained: the fast-moving and provider-variant values.
// `service_tier`, `reasoning_effort`, `tools[].type`, `tool_calls[].type` and
// `modalities[]` have all grown new values within the last year, and a proxy one
// release behind must not 400 traffic the upstream would accept.
//
// The bias is deliberate and asymmetric: rejecting a request the upstream would
// have accepted is worse than forwarding one it will reject itself, because only
// the first is a failure the proxy invented. Undeclared fields passing through is
// the same principle -- a field OpenAI ships next month needs no change here.
//
// Not expressible in this shape, and left to the upstream: `stream_options`
// requires `stream: true`; `content` is required for a user or system message but
// nullable on a tool-calling assistant one; `tool_call_id` is required only for
// `role: tool`; `logit_bias` values are bounded -100..100. Those are cross-field
// and map-value rules a per-field tree has no vocabulary for.
//
// OFFLOADABLE FIELDS are only the free text: the two forms of message content,
// an image URL (data URIs are large), tool-call arguments, and a tool
// description. Everything carrying a value constraint is inline by construction,
// which the builder enforces -- a value the proxy has to compare against a list
// must be one the proxy can read.

const FieldSchema* buildOpenAiChatCompletionsRequestSchema(SchemaBuilder& b) {
  // Built bottom-up: a parent can only be declared once its children have
  // pointers. Shared subtrees are declared once and referenced from every parent.

  // One element of the multi-part content array. Reached from a message's content
  // and from `prediction.content`.
  const FieldSchema* content_part = b.object({
      {"type", Required, b.str({"text", "image_url", "input_audio", "file", "refusal"})},
      // The prompt, and the first thing to stream: this is the field the whole
      // external-buffer design exists for.
      {"text", b.offloadableStr(StreamOrder::Prompt)},
      {"refusal", b.str()},
      {"image_url", b.object({
                        {"url", Required, b.offloadableStr(StreamOrder::Prompt)},
                        {"detail", b.str({"auto", "low", "high"})},
                    })},
      // Base64 blobs and file handles: the shapes churn and the bytes are opaque
      // to routing, so only the discriminator above is held.
      {"input_audio", b.anyObject()},
      {"file", b.anyObject()},
  });

  // A string, or the multi-part array. Null is handled by the validator's rule for
  // an optional field rather than by an alternative here, which is what covers a
  // tool-calling assistant message.
  const FieldSchema* content =
      b.oneOf({b.offloadableStr(StreamOrder::Prompt), b.array(content_part)});

  const FieldSchema* tool_call = b.object({
      {"id", Required, b.str()},
      // Not an enum: `custom` joined `function` recently, and more will follow.
      {"type", b.str()},
      {"function", b.object({
                       {"name", b.str()},
                       // Caller-authored, frequently large, and not even reliably
                       // valid JSON -- a model can emit a truncated argument blob.
                       {"arguments", b.offloadableStr(StreamOrder::Tool)},
                   })},
      {"custom", b.anyObject()},
  });

  const FieldSchema* message = b.object({
      {"role", Required, b.str({"system", "user", "assistant", "tool", "developer", "function"})},
      // Optional rather than required: absent on an assistant turn that carries
      // only tool_calls. Which roles require it is a cross-field rule.
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
                       {"description", b.offloadableStr(StreamOrder::Tool)},
                       {"strict", b.boolean()},
                       // Caller-supplied JSON Schema, carried through untouched: a
                       // proxy has no business modelling JSON Schema, and doing so
                       // would put every future keyword on this file's critical
                       // path.
                       {"parameters", b.anyJson()},
                   })},
      {"custom", b.anyObject()},
  });

  return b.object({
      {"model", Required, b.str()},
      {"messages", Required, b.array(message, /*min_items=*/1)},

      // Documented, inclusive, and unchanged since launch.
      {"temperature", b.number(0.0, 2.0)},
      {"top_p", b.number(0.0, 1.0)},
      {"presence_penalty", b.number(-2.0, 2.0)},
      {"frequency_penalty", b.number(-2.0, 2.0)},
      {"top_logprobs", b.integer(0, 20)},
      // Lower bound only: the upper caps are model-specific and move.
      {"n", b.integer(/*min_value=*/1)},
      {"max_tokens", b.integer(/*min_value=*/1)}, // Deprecated, still widely sent.
      {"max_completion_tokens", b.integer(/*min_value=*/1)},
      {"seed", b.integer()},
      {"logprobs", b.boolean()},

      // Envoy acts on this one -- it decides whether the response is SSE -- so it
      // is held to a strict boolean rather than left loose.
      {"stream", b.boolean()},
      {"stream_options", b.object({{"include_usage", b.boolean()}})},

      {"stop", b.oneOf({b.str(), b.array(b.str())})},
      {"tools", b.array(tool)},
      {"tool_choice", b.oneOf({b.str({"none", "auto", "required"}),
                               b.object({
                                   {"type", Required, b.str()},
                                   {"function", b.object({{"name", Required, b.str()}})},
                                   {"custom", b.anyObject()},
                               })})},
      {"parallel_tool_calls", b.boolean()},
      {"response_format", b.object({
                              {"type", Required, b.str({"text", "json_object", "json_schema"})},
                              {"json_schema", b.object({
                                                  {"name", Required, b.str()},
                                                  {"description", b.str()},
                                                  {"strict", b.boolean()},
                                                  // Caller-supplied, as above.
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
      // Dynamic key -> value maps. This shape has no notion of a schema for an
      // object's values, so keys and values are both unconstrained; not worth a
      // map-value construct for two fields nothing routes on.
      {"logit_bias", b.anyObject()},
      {"metadata", b.anyObject()},
      // Values churn faster than an Envoy release, so the type is held and the
      // value is not.
      {"service_tier", b.str()},
      {"reasoning_effort", b.str()},
  });
}

const FieldSchema* buildOpenAiChatCompletionsResponseSchema(SchemaBuilder& b) {
  // See the header for why this is empty.
  return b.anyObject();
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
