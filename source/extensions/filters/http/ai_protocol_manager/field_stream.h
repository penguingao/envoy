#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "envoy/buffer/buffer.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/coroutine/async_queue.h"
#include "source/common/coroutine/task.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

struct BufferSizeFunc {
  uint64_t operator()(const Buffer::OwnedImpl& buf) const { return buf.length(); }
};

using ByteQueue = Coroutine::AsyncQueue<Buffer::OwnedImpl, BufferSizeFunc>;
using ByteQueuePtr = std::shared_ptr<ByteQueue>;

// FieldStream represents an asynchronous stream of data chunks for a single
// JSON field path. It provides recv() to pull data from a sequence of input queues and
// forward() to push data to downstream filters or the sink.
class FieldStream {
public:
  explicit FieldStream(std::string json_path);
  FieldStream(std::string json_path, ByteQueuePtr queue);
  FieldStream(std::string json_path, std::deque<ByteQueuePtr> input_queues);
  FieldStream(std::string json_path, std::deque<ByteQueuePtr> input_queues,
              ByteQueuePtr output_queue);
  ~FieldStream();

  FieldStream(FieldStream&&) noexcept = default;
  FieldStream& operator=(FieldStream&&) noexcept = default;
  FieldStream(const FieldStream&) = default;
  FieldStream& operator=(const FieldStream&) = default;

  absl::string_view jsonPath() const { return json_path_; }

  // Receives the next data chunk from the active input queue. Returns std::nullopt at EOF.
  Coroutine::Task<absl::StatusOr<std::optional<Buffer::OwnedImpl>>> recv();

  // Forwards a data chunk downstream. When end_stream is true, closes the stream.
  Coroutine::Task<absl::Status> forward(Buffer::Instance& chunk, bool end_stream = false);
  Coroutine::Task<absl::Status> forward(Buffer::OwnedImpl chunk, bool end_stream = false);

  // Synchronous push chunk into the output queue (or primary input queue).
  void pushChunk(Buffer::OwnedImpl chunk, bool end_stream);

  // Closes all managed queues.
  void close();

  // Drains all currently queued chunks synchronously.
  Buffer::OwnedImpl drainAll();

  const std::deque<ByteQueuePtr>& inputQueues() const { return input_queues_; }
  std::deque<ByteQueuePtr>& inputQueues() { return input_queues_; }
  ByteQueuePtr outputQueue() const { return output_queue_; }
  void setOutputQueue(ByteQueuePtr q) { output_queue_ = std::move(q); }

private:
  std::string json_path_;
  std::deque<ByteQueuePtr> input_queues_;
  ByteQueuePtr output_queue_{nullptr};
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
