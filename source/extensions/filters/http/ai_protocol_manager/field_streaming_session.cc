#include "source/extensions/filters/http/ai_protocol_manager/field_streaming_session.h"

#include <utility>

#include "source/extensions/filters/http/ai_protocol_manager/filter_manager.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

FieldStreamHandle::FieldStreamHandle(std::string json_path, std::deque<ByteQueuePtr> in_queues,
                                     ByteQueuePtr out_queue, std::shared_ptr<bool> forward_called)
    : json_path_(std::move(json_path)),
      stream_(json_path_, std::move(in_queues), std::move(out_queue)),
      forward_called_(std::move(forward_called)) {}

Coroutine::Task<absl::StatusOr<std::optional<Buffer::OwnedImpl>>> FieldStreamHandle::recv() {
  return stream_.recv();
}

Coroutine::Task<absl::Status> FieldStreamHandle::forward(Buffer::Instance& chunk, bool end_stream) {
  if (forward_called_ != nullptr) {
    *forward_called_ = true;
  }
  return stream_.forward(chunk, end_stream);
}

Coroutine::Task<absl::Status> FieldStreamHandle::forward(Buffer::OwnedImpl chunk, bool end_stream) {
  if (forward_called_ != nullptr) {
    *forward_called_ = true;
  }
  return stream_.forward(std::move(chunk), end_stream);
}

FieldStreamingSession::FieldStreamingSession(FilterManager& manager, size_t filter_index,
                                             std::vector<std::string> registered_paths,
                                             bool is_stage_terminator)
    : manager_(manager), filter_index_(filter_index), is_stage_terminator_(is_stage_terminator),
      registered_paths_(std::move(registered_paths)) {
  if (registered_paths_.empty()) {
    is_done_ = true;
    incoming_queue_.close();
  }
  for (const auto& path : registered_paths_) {
    manager_.registerFieldStreamingSession(path, this);
  }
}

FieldStreamingSession::~FieldStreamingSession() {
  for (const auto& path : registered_paths_) {
    auto it = inflight_streams_.find(path);
    if (!is_stage_terminator_) {
      if (it != inflight_streams_.end()) {
        if (it->second.published) {
          // If the filter did not explicitly close out_queue with end_stream=true, close it
          // so any downstream reader can advance past out_queue to in_queues.
          if (!it->second.out_queue->closed()) {
            it->second.out_queue->close();
          }
        } else if (it->second.fetched) {
          // The filter explicitly retrieved this field from fetch() but omitted calling publish(),
          // indicating an intentional field drop.
          manager_.markNextStreamingSessionDone(path, filter_index_ + 1);
        } else {
          // The filter early returned before reaching this field in fetch().
          // Forward in_queues directly downstream to maintain stream continuity.
          manager_.forwardStreamChannel(path, std::move(it->second.in_queues), filter_index_ + 1);
        }
      }
    }
    manager_.unregisterFieldStreamingSession(path, this);
  }
}

Coroutine::Task<absl::StatusOr<std::optional<FieldStreamHandle>>> FieldStreamingSession::fetch() {
  auto handle_or = co_await incoming_queue_.pop();
  if (handle_or.ok() && handle_or->has_value()) {
    std::string path = std::string((**handle_or).jsonPath());
    auto it = inflight_streams_.find(path);
    if (it != inflight_streams_.end()) {
      it->second.fetched = true;
    }
  }
  co_return handle_or;
}

void FieldStreamingSession::publish(const FieldStreamHandle& handle) {
  std::string path = std::string(handle.jsonPath());
  auto it = inflight_streams_.find(path);
  if (it != inflight_streams_.end() && !it->second.published) {
    it->second.published = true;

    if (!is_stage_terminator_) {
      std::deque<ByteQueuePtr> forwarded_queues;
      forwarded_queues.push_back(it->second.out_queue);
      for (const auto& q : it->second.in_queues) {
        forwarded_queues.push_back(q);
      }
      manager_.forwardStreamChannel(path, std::move(forwarded_queues), filter_index_ + 1);
    }
  }
}

void FieldStreamingSession::insertStreamChannel(std::string path,
                                                std::deque<ByteQueuePtr> in_queues) {
  auto out_queue = std::make_shared<ByteQueue>();
  auto forward_called = std::make_shared<bool>(false);

  inflight_streams_[path] =
      InflightStream{in_queues, out_queue, forward_called, /*published=*/false};
  FieldStreamHandle handle(path, std::move(in_queues), out_queue, forward_called);
  incoming_queue_.tryPush(std::move(handle));
}

void FieldStreamingSession::markDone() {
  is_done_ = true;
  incoming_queue_.close();
  if (is_stage_terminator_) {
    for (const auto& path : registered_paths_) {
      manager_.unregisterFieldStreamingSession(path, this);
    }
  }
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
