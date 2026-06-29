#pragma once

#include "envoy/buffer/buffer.h"
#include "envoy/common/pure.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

/**
 * An interface for an external buffer that supports asynchronous I/O and flow control.
 * This is used to offload HTTP payloads to a bounded-memory external storage (like disk or external
 * service) and stream them back-in.
 */
class ExternalBuffer {
public:
  virtual ~ExternalBuffer() = default;

  class WriteCallback {
  public:
    virtual ~WriteCallback() = default;
    /**
     * Called when the write operation is complete.
     * @param status The status of the write operation.
     */
    virtual void onWriteComplete(absl::Status status) PURE;
  };

  class ReadCallback {
  public:
    virtual ~ReadCallback() = default;
    /**
     * Called when the read operation is complete.
     * @param data The data read from the buffer, or an error status.
     */
    virtual void onReadComplete(absl::StatusOr<Buffer::InstancePtr> data) PURE;
  };

  /**
   * Appends data to the external buffer.
   * @param data The data to append. The buffer may drain/consume this data.
   * @param callback The callback to invoke when the write is complete.
   */
  virtual void write(Buffer::Instance& data, WriteCallback& callback) PURE;

  /**
   * Reads a slice of data from the external buffer.
   * @param offset The byte offset to start reading from.
   * @param length The number of bytes to read.
   * @param callback The callback to invoke when the read is complete.
   */
  virtual void read(uint64_t offset, uint64_t length, ReadCallback& callback) PURE;

  /**
   * @return the total number of bytes currently stored in the buffer.
   */
  virtual uint64_t size() const PURE;

  /**
   * Resets the buffer, clearing all stored data.
   */
  virtual void reset() PURE;
};

using ExternalBufferPtr = std::unique_ptr<ExternalBuffer>;

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
