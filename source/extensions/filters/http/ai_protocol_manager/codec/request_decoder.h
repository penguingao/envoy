#pragma once

#include <memory>
#include <string>

#include "envoy/http/header_map.h"

#include "source/common/common/logger.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/protocol_classifier.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

struct DecoderConfig {
  // JSON-body fields whose serialized size is at or below this threshold are
  // stored as Inline PayloadRefs; larger fields are stored as Buffered refs.
  size_t max_inline_bytes{4096};

  // Hard limit: requests whose body exceeds this size are rejected with
  // ResourceExhausted. Enforced incrementally in feed() so memory never
  // grows beyond this ceiling. Default: 4 MB.
  size_t max_body_bytes{4 * 1024 * 1024};

  // Soft limit: bodies at or below this size get full per-element byte-range
  // capture (messages[], tools[]). Bodies above this limit still have their
  // scalar fields extracted (model, stream, sampling params) but individual
  // element PayloadRefs are not populated — avoids the 2× memory cost of
  // copying large element bytes out of the slab chain. Default: 256 KB.
  size_t max_element_capture_bytes{256 * 1024};
};

// RequestDecoder translates an HTTP request (headers + streamed body) into a
// fully-populated AiRequest. It is driven by the outer Envoy filter's
// decodeHeaders / decodeData / decodeTrailers callbacks.
//
// Typical usage:
//
//   FilterHeadersStatus decodeHeaders(RequestHeaderMap& hdrs, bool end_stream) {
//     decoder_.onHeaders(hdrs);
//     if (end_stream) { decoder_.onEndStream(); dispatch(); }
//     return StopIteration;   // wait for body
//   }
//   FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) {
//     decoder_.onData(data.toString());
//     if (end_stream) { decoder_.onEndStream(); dispatch(); }
//     return StopIterationAndBuffer;
//   }
//   void dispatch() {
//     auto req = decoder_.take();  // moves AiRequest out
//     sub_chain_.run(*req);
//   }
class RequestDecoder : public Logger::Loggable<Logger::Id::filter> {
public:
  RequestDecoder(const DecoderConfig& config, PayloadStore& store);
  ~RequestDecoder();

  RequestDecoder(const RequestDecoder&) = delete;
  RequestDecoder& operator=(const RequestDecoder&) = delete;

  // Called once when all request headers have arrived. Populates verb, path,
  // path_params, query_params, and headers on the internal AiRequest; runs the
  // classifier; and selects (or skips) the body parser. Returns an error if
  // the request cannot be classified.
  absl::Status onHeaders(const Http::RequestHeaderMap& headers);

  // Called for each body data chunk. No-op when no body is expected (bodiless
  // verbs) or when the state is not ParsingBody.
  absl::Status onData(absl::string_view chunk);

  // Called when request trailers arrive. AI protocols do not use request
  // trailers; this is a no-op that delegates to onEndStream.
  absl::Status onTrailers(const Http::RequestTrailerMap& trailers);

  // Called when the request stream ends (either by end_stream flag on headers
  // or by end_stream on the last data frame). Finalizes body parsing, validates
  // the result, and makes the AiRequest available via take().
  absl::Status onEndStream();

  // True while the decoder is in ParsingInferenceBody or ParsingAgentBody —
  // i.e. the caller must supply body chunks before calling onEndStream.
  bool needsBody() const;

  // Returns the classified protocol kind.
  ProtocolKind protocol() const { return request_.protocol; }

  // Moves the completed AiRequest out of the decoder. Valid only after a
  // successful onEndStream(); returns FailedPrecondition otherwise. Resets
  // the decoder to AwaitingHeaders so it can be reused for the next request
  // (though in practice one RequestDecoder per stream is typical).
  absl::StatusOr<AiRequest> take();

private:
  // ── State machine ──────────────────────────────────────────────────────────

  enum class DecodeState {
    AwaitingHeaders,
    BodilessComplete,     // no body expected; AiRequest fully populated by onHeaders
    ParsingInferenceBody, // accumulating REST-JSON body bytes
    ParsingAgentBody,     // buffering JSON-RPC body for end-of-stream parse
    BodyComplete,         // onEndStream done; take() is valid
    Error,
  };

  // ── Inner body parsers (defined in request_decoder.cc) ────────────────────

  // Accumulates the full inference JSON body in a buffer and parses it at
  // onEndStream using Json::Factory::loadFromString. Extracts scalar sampling
  // params, and stores each messages/tools element as a PayloadRef.
  class InferenceBodyParser;

  // Buffers the full JSON-RPC body and parses it at onEndStream using
  // nlohmann::json. Extracts the envelope (id, method), re-classifies with
  // rpc_method to determine AgentInvocation, and populates AgentPayload params.
  class AgentBodyParser;

  // ── Helpers ────────────────────────────────────────────────────────────────

  void         populateQueryParams(absl::string_view path);
  absl::Status populateBodilessPayload(const ClassifyResult& result);

  // ── State ──────────────────────────────────────────────────────────────────

  const DecoderConfig& config_;
  PayloadStore&        payload_store_;
  DecodeState          state_{DecodeState::AwaitingHeaders};
  AiRequest            request_;

  std::unique_ptr<InferenceBodyParser> inference_parser_;
  std::unique_ptr<AgentBodyParser>     agent_parser_;
};

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
