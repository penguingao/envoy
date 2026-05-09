#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_payload.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

std::string PayloadRef::toString() const {
  switch (storage_) {
  case Storage::Inline:
    return inline_data_;
  case Storage::Buffered:
    if (buffered_data_ && buffered_data_->length() > 0) {
      return buffered_data_->toString();
    }
    return {};
  case Storage::External:
    PANIC("External PayloadRef must be materialized through PayloadStore::fetch()");
  }
  return {};
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
