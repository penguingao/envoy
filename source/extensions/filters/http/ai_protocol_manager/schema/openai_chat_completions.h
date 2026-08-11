#pragma once

#include "source/extensions/filters/http/ai_protocol_manager/schema/field_schema.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// The OpenAI Chat Completions request schema. Also the canonical request schema
// for now, so normalizing an OpenAI payload is an identity transform.
const FieldSchema* buildOpenAiChatCompletionsRequestSchema(SchemaBuilder& builder);

// The OpenAI Chat Completions response schema.
//
// Deliberately empty -- it says the response is an object and nothing more. The
// encode path is not wired, and a streaming response is a sequence of SSE chunks
// rather than one document, so response validation will likely want a
// chunk-oriented shape. This exists so the response side of the registry has a
// real object to hand out.
const FieldSchema* buildOpenAiChatCompletionsResponseSchema(SchemaBuilder& builder);

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
