#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_payload.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

std::string PayloadRef::toString() const {
  if (storage_ == Storage::Inline) {
    return inline_data_;
  }
  if (buffered_data_ && buffered_data_->length() > 0) {
    return buffered_data_->toString();
  }
  return {};
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
