#include "source/extensions/filters/http/ai_protocol_manager/field_stream.h"

#include <utility>

#include "source/common/coroutine/status_macros.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

FieldStream::FieldStream(std::string json_path) : json_path_(std::move(json_path)) {
  auto queue = std::make_shared<ByteQueue>();
  input_queues_.push_back(queue);
  output_queue_ = queue;
}

FieldStream::FieldStream(std::string json_path, ByteQueuePtr queue)
    : json_path_(std::move(json_path)), output_queue_(queue) {
  if (queue != nullptr) {
    input_queues_.push_back(std::move(queue));
  }
}

FieldStream::FieldStream(std::string json_path, std::deque<ByteQueuePtr> input_queues)
    : json_path_(std::move(json_path)), input_queues_(std::move(input_queues)) {
  if (!input_queues_.empty()) {
    output_queue_ = input_queues_.back();
  }
}

FieldStream::FieldStream(std::string json_path, std::deque<ByteQueuePtr> input_queues,
                         ByteQueuePtr output_queue)
    : json_path_(std::move(json_path)), input_queues_(std::move(input_queues)),
      output_queue_(std::move(output_queue)) {}

FieldStream::~FieldStream() = default;

Coroutine::Task<absl::StatusOr<std::optional<Buffer::OwnedImpl>>> FieldStream::recv() {
  while (!input_queues_.empty()) {
    auto current_queue = input_queues_.front();
    if (current_queue == nullptr) {
      input_queues_.pop_front();
      continue;
    }
    auto item_or = co_await current_queue->pop();
    if (!item_or.ok()) {
      co_return item_or.status();
    }
    if (item_or->has_value()) {
      co_return item_or;
    }
    // Front queue reached EOF (closed and drained). Drop it and advance to next queue in sequence.
    input_queues_.pop_front();
  }
  co_return std::nullopt;
}

Coroutine::Task<absl::Status> FieldStream::forward(Buffer::Instance& chunk, bool end_stream) {
  Buffer::OwnedImpl owned;
  owned.move(chunk);
  co_return co_await forward(std::move(owned), end_stream);
}

Coroutine::Task<absl::Status> FieldStream::forward(Buffer::OwnedImpl chunk, bool end_stream) {
  if (output_queue_ == nullptr) {
    if (!input_queues_.empty()) {
      output_queue_ = input_queues_.back();
    } else {
      output_queue_ = std::make_shared<ByteQueue>();
      input_queues_.push_back(output_queue_);
    }
  }
  if (chunk.length() > 0) {
    CO_RETURN_IF_ERROR(co_await output_queue_->push(std::move(chunk)));
  }
  if (end_stream) {
    for (auto& q : input_queues_) {
      if (q != nullptr && q != output_queue_) {
        while (q->tryPop()) {
        }
        q->close();
      }
    }
    output_queue_->close();
  }
  co_return absl::OkStatus();
}

void FieldStream::pushChunk(Buffer::OwnedImpl chunk, bool end_stream) {
  if (output_queue_ == nullptr) {
    if (!input_queues_.empty()) {
      output_queue_ = input_queues_.back();
    } else {
      output_queue_ = std::make_shared<ByteQueue>();
      input_queues_.push_back(output_queue_);
    }
  }
  if (chunk.length() > 0) {
    output_queue_->tryPush(std::move(chunk));
  }
  if (end_stream) {
    for (auto& q : input_queues_) {
      if (q != nullptr && q != output_queue_) {
        while (q->tryPop()) {
        }
        q->close();
      }
    }
    output_queue_->close();
  }
}

void FieldStream::close() {
  for (auto& q : input_queues_) {
    if (q != nullptr) {
      q->close();
    }
  }
  if (output_queue_ != nullptr) {
    output_queue_->close();
  }
}

Buffer::OwnedImpl FieldStream::drainAll() {
  Buffer::OwnedImpl accumulated;
  while (!input_queues_.empty()) {
    auto current = input_queues_.front();
    if (current != nullptr) {
      while (auto item = current->tryPop()) {
        accumulated.move(*item);
      }
    }
    input_queues_.pop_front();
  }
  if (output_queue_ != nullptr) {
    while (auto item = output_queue_->tryPop()) {
      accumulated.move(*item);
    }
  }
  return accumulated;
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
