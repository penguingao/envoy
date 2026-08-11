#pragma once

#include "source/extensions/filters/http/ai_protocol_manager/schema/field_schema.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// Builds the OpenAI Chat Completions request schema into `builder` and returns
// its root. Called once per process, from the schema registry.
//
// This is also the canonical request schema for now, so a route asking to
// normalize an OpenAI payload is asking for an identity transform. A second
// provider gets its own schema plus a field map into this one.
const FieldSchema* buildOpenAiChatCompletionsRequestSchema(SchemaBuilder& builder);

// Builds the OpenAI Chat Completions response schema.
//
// Deliberately empty: it declares that the response is a JSON object and nothing
// more. The encode path is not wired at all yet, and a streaming response is a
// sequence of SSE chunks rather than one JSON document, so response validation
// will likely need a chunk-oriented shape rather than a document schema.
// Inventing constraints nothing exercises would be untested surface; this exists
// so the response side of the registry has a real object to hand out.
const FieldSchema* buildOpenAiChatCompletionsResponseSchema(SchemaBuilder& builder);

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
