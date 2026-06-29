#pragma once

#include "source/common/buffer/buffer_impl.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

class InMemoryBuffer : public ExternalBuffer {
public:
  InMemoryBuffer() = default;

  // ExternalBuffer
  void write(Buffer::Instance& data, WriteCallback& callback) override;
  void read(uint64_t offset, uint64_t length, ReadCallback& callback) override;
  uint64_t size() const override { return buffer_.length(); }
  void reset() override { buffer_.drain(buffer_.length()); }

private:
  Buffer::OwnedImpl buffer_;
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
