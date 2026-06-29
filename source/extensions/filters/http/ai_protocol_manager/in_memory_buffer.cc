#include "source/extensions/filters/http/ai_protocol_manager/in_memory_buffer.h"

#include <algorithm>

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

void InMemoryBuffer::write(Buffer::Instance& data, WriteCallback& callback) {
  // Move data into our internal buffer.
  buffer_.move(data);
  // Synchronously signal completion.
  callback.onWriteComplete(absl::OkStatus());
}

void InMemoryBuffer::read(uint64_t offset, uint64_t length, ReadCallback& callback) {
  if (offset + length > buffer_.length()) {
    callback.onReadComplete(absl::InvalidArgumentError("Read range out of bounds"));
    return;
  }

  auto output = std::make_unique<Buffer::OwnedImpl>();
  if (length > 0) {
    auto reservation = output->reserveSingleSlice(length);
    buffer_.copyOut(offset, length, reservation.slice().mem_);
    reservation.commit(length);
  }

  callback.onReadComplete(std::move(output));
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
