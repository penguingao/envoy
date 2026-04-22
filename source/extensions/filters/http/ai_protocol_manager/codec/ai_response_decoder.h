#pragma once

#include "envoy/buffer/buffer.h"

#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_response.h"

#include "absl/status/status.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

// DESIGN.md §4.6 + §6 — upstream HTTP response → AiResponse + downstream
// body. Symmetric to AiRequestEncoder on the request side: the dispatch
// owns the AsyncClient call, hands the upstream body here, and gets back a
// converted body (in the downstream client's expected schema) plus a
// populated AiResponse summary.
//
// V0 is non-streaming: the entire upstream body is passed in one shot to
// decodeFullBody. Streaming SSE responses get a sibling onChunk / onEnd
// API in a later phase; the abstract base will gain those methods then.
class AiResponseDecoder {
public:
  virtual ~AiResponseDecoder() = default;

  // Convert a complete upstream response body into the downstream-shape
  // body and populate `ai_response.summary`. The decoder does not touch
  // headers; the caller updates Content-Length / Content-Type after this
  // returns based on `out_body.length()` and the decoder's known content
  // type. Returns InvalidArgument on parse failure; callers should fall
  // back to a 502 when this is non-OK.
  virtual absl::Status decodeFullBody(absl::string_view upstream_body,
                                      AiResponse& ai_response,
                                      Buffer::Instance& out_body) = 0;
};

using AiResponseDecoderPtr = std::unique_ptr<AiResponseDecoder>;

// Pure pass-through: the upstream body is the downstream body, byte-for-byte.
// Used for OPENAI_PASSTHROUGH where the upstream already speaks the schema
// the downstream client wants.
class PassThroughResponseDecoder : public AiResponseDecoder {
public:
  absl::Status decodeFullBody(absl::string_view upstream_body, AiResponse& ai_response,
                              Buffer::Instance& out_body) override;
};

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
