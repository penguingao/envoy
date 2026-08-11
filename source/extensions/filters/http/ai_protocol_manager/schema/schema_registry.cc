#include "source/extensions/filters/http/ai_protocol_manager/schema/schema_registry.h"

#include "source/common/common/assert.h"
#include "source/common/singleton/const_singleton.h"
#include "source/extensions/filters/http/ai_protocol_manager/schema/openai_chat_completions.h"
#include "source/extensions/filters/http/ai_protocol_manager/schema/tree_payload_schema.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

// Owns every schema this binary knows.
//
// Reached through ConstSingleton, which default-constructs it in place, so nothing
// is ever moved and the schemas' interior pointers need no argument made for them.
// (CONSTRUCT_ON_FIRST_USE would move-construct from its argument; that move
// happens to be safe here, but relying on it is the kind of subtlety worth
// designing out.) Initialization is thread-safe by C++11 magic statics, and the
// tables are read-only afterwards, so concurrent reads need no synchronization.
class SchemaTable {
public:
  const PayloadSchema* request(Schema schema) const {
    switch (schema) {
    case PerRouteProto::OPENAI_CHAT_COMPLETIONS:
      return &openai_chat_completions_request_;
    case PerRouteProto::UNSPECIFIED:
      return nullptr;
    default:
      // Proto validation (defined_only) rules this out of a configuration. A
      // schema this binary has no table for is skipped rather than rejected:
      // rejecting every request because a newer control plane named something
      // unfamiliar is worse than forwarding it.
      return nullptr;
    }
  }

  const PayloadSchema* response(Schema schema) const {
    switch (schema) {
    case PerRouteProto::OPENAI_CHAT_COMPLETIONS:
      return &openai_chat_completions_response_;
    case PerRouteProto::UNSPECIFIED:
      return nullptr;
    default:
      return nullptr;
    }
  }

private:
  // A lookup is a switch rather than a map: there are two values, a switch is a
  // compare, and adding a provider means editing something a reviewer sees.
  // Generated proto enums carry sentinel enumerators, so -Wswitch cannot be
  // relied on to catch a new value -- schema_registry_test.cc drives
  // Schema_descriptor() to enforce completeness instead.
  const TreePayloadSchema openai_chat_completions_request_{"openai_chat_completions",
                                                           buildOpenAiChatCompletionsRequestSchema,
                                                           openAiChatCompletionsStreamOrder};
  const TreePayloadSchema openai_chat_completions_response_{
      "openai_chat_completions_response", buildOpenAiChatCompletionsResponseSchema};
};

} // namespace

const PayloadSchema* requestSchemaFor(Schema schema) {
  return ConstSingleton<SchemaTable>::get().request(schema);
}

const PayloadSchema* responseSchemaFor(Schema schema) {
  return ConstSingleton<SchemaTable>::get().response(schema);
}

const PayloadSchema& canonicalRequestSchema() {
  const PayloadSchema* canonical = requestSchemaFor(PerRouteProto::OPENAI_CHAT_COMPLETIONS);
  ASSERT(canonical != nullptr);
  return *canonical;
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
