#pragma once

#include <cstdint>
#include <string>

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

// AiResponse — the normalized response envelope used by the AI sub-chain.
//
// R1/R2/R3 phases operate on AiResponse / AiResponseChunk rather than raw
// HTTP buffers, exactly as Q1/Q2 phases operate on AiRequest.
//
// v0: only the fields required for local replies (auth rejection, early errors)
// are defined. Streaming response fields (tokens, finish_reason, usage,
// tool_calls) are added when the response pipeline is implemented.
struct AiResponse {
  // HTTP status code to use when the chain synthesizes a local reply via
  // AiFilterCallbacks::sendLocalReply(). The outer AiProtocolManagerFilter
  // passes this to decoder_callbacks_->sendLocalReply().
  int http_status{200};

  // Pre-serialized response body. Sent verbatim as the HTTP response body
  // when http_status indicates an early termination (4xx / 5xx).
  std::string body;
};

// AiResponseChunk — one streaming output unit (SSE event / delta object).
// Full definition deferred to the response-path implementation.
struct AiResponseChunk {};

// AiItem — one addressable element of the request payload.
// Full definition deferred to the Q2 item-iteration implementation.
struct AiItem {};

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
