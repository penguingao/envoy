#pragma once

#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.h"

#include "source/extensions/filters/http/ai_protocol_manager/schema/payload_schema.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

using PerRouteProto =
    envoy::extensions::filters::http::ai_protocol_manager::v3::AiProtocolManagerPerRoute;
// The schema a route declares its payload follows. UNSPECIFIED means the route
// declared nothing, i.e. it is not an AI endpoint.
using Schema = PerRouteProto::Schema;

// The request schema for `schema`, or nullptr when there is nothing to hold a
// payload to: UNSPECIFIED, or -- defensively -- a schema this binary has no table
// for.
//
// The returned object lives for the process's lifetime, so a caller may cache the
// pointer for the duration of a stream even across a route re-resolution.
const PayloadSchema* requestSchemaFor(Schema schema);

// The response schema for `schema`, or nullptr. Nothing consumes this yet: the
// encode path is not wired (filter.h).
const PayloadSchema* responseSchemaFor(Schema schema);

// The canonical request schema -- what a payload is normalized into when a route
// asks for it.
//
// Canonical is the OpenAI Chat Completions shape for now, so this is literally the
// same object requestSchemaFor(OPENAI_CHAT_COMPLETIONS) returns and normalization
// is an identity transform. A test pins that identity, so a future divergence has
// to be a deliberate edit rather than a drift.
const PayloadSchema& canonicalRequestSchema();

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
