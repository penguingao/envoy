#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "source/common/coroutine/async_queue.h"
#include "source/common/coroutine/task.h"
#include "source/extensions/filters/http/ai_protocol_manager/field_stream.h"

#include "absl/container/flat_hash_map.h"
#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

class FilterManager;
class FieldStreamingSession;

// FieldStreamHandle provides an AI filter access to stream chunks for a single
// registered field path.
class FieldStreamHandle {
public:
  FieldStreamHandle(std::string json_path, std::deque<ByteQueuePtr> in_queues,
                    ByteQueuePtr out_queue, std::shared_ptr<bool> forward_called);
  ~FieldStreamHandle() = default;

  FieldStreamHandle(FieldStreamHandle&&) noexcept = default;
  FieldStreamHandle& operator=(FieldStreamHandle&&) noexcept = default;
  FieldStreamHandle(const FieldStreamHandle&) = default;
  FieldStreamHandle& operator=(const FieldStreamHandle&) = default;

  absl::string_view jsonPath() const { return json_path_; }

  // Receives the next data chunk from upstream. Returns std::nullopt at EOF.
  Coroutine::Task<absl::StatusOr<std::optional<Buffer::OwnedImpl>>> recv();

  // Forwards a data chunk downstream. When end_stream is true, closes the stream.
  Coroutine::Task<absl::Status> forward(Buffer::Instance& chunk, bool end_stream = false);
  Coroutine::Task<absl::Status> forward(Buffer::OwnedImpl chunk, bool end_stream = false);

private:
  friend class FieldStreamingSession;

  std::string json_path_;
  FieldStream stream_;
  std::shared_ptr<bool> forward_called_;
};

// FieldStreamingSession coordinates streaming of multiple registered fields in canonical
// schema order for a single filter stage. It manages the in-flight stream queues.
class FieldStreamingSession {
public:
  FieldStreamingSession(FilterManager& manager, size_t filter_index,
                        std::vector<std::string> registered_paths,
                        bool is_stage_terminator = false);
  ~FieldStreamingSession();

  // FieldStreamingSession is neither copyable nor movable.
  FieldStreamingSession(FieldStreamingSession&&) = delete;
  FieldStreamingSession& operator=(FieldStreamingSession&&) = delete;
  FieldStreamingSession(const FieldStreamingSession&) = delete;
  FieldStreamingSession& operator=(const FieldStreamingSession&) = delete;

  // Fetches the next FieldStreamHandle for this pass in canonical schema order.
  // Returns std::nullopt when all registered fields have been delivered.
  Coroutine::Task<absl::StatusOr<std::optional<FieldStreamHandle>>> fetch();

  // Publishes the stream to the next filter in the chain (or sink).
  // If the filter omits calling publish(), the field is dropped from subsequent processing.
  void publish(const FieldStreamHandle& handle);

private:
  friend class FilterManager;

  struct InflightStream {
    std::deque<ByteQueuePtr> in_queues;
    ByteQueuePtr out_queue;
    std::shared_ptr<bool> forward_called;
    bool published{false};
    bool fetched{false};
  };

  void insertStreamChannel(std::string path, std::deque<ByteQueuePtr> in_queues);
  void markDone();
  size_t filterIndex() const { return filter_index_; }
  const std::vector<std::string>& registeredPaths() const { return registered_paths_; }

  FilterManager& manager_;
  const size_t filter_index_;
  const bool is_stage_terminator_{false};
  std::vector<std::string> registered_paths_;

  Coroutine::AsyncQueue<FieldStreamHandle> incoming_queue_;
  absl::flat_hash_map<std::string, InflightStream> inflight_streams_;
  bool is_done_{false};
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
