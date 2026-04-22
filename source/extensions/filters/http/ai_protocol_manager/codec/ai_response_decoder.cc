#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_response_decoder.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

absl::Status PassThroughResponseDecoder::decodeFullBody(absl::string_view upstream_body,
                                                         AiResponse& /*ai_response*/,
                                                         Buffer::Instance& out_body) {
  // No conversion. The upstream already speaks the downstream schema, so we
  // just hand the bytes through. AiResponse summary stays monostate — the
  // mapper for the source schema (e.g. an OpenAI response mapper) would
  // populate it if a future filter wants typed access.
  out_body.add(upstream_body);
  return absl::OkStatus();
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
