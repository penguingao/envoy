#include "source/extensions/filters/http/ai_protocol_manager/filter_manager.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "source/common/buffer/buffer_impl.h"
#include "source/common/coroutine/dispatcher_executor.h"
#include "source/common/coroutine/launch.h"
#include "source/common/coroutine/status_macros.h"
#include "source/extensions/filters/http/ai_protocol_manager/serializer.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

FilterManager::FilterManager(std::vector<AiFilterPtr> filters, JsonWithExtBuf request_json,
                             BufferManager* buffer_manager, Event::Dispatcher& dispatcher,
                             const PayloadSchema* schema, DataInjector data_injector)
    : filters_(std::move(filters)), request_json_(std::move(request_json)),
      buffer_manager_(buffer_manager), dispatcher_(dispatcher),
      executor_(std::make_shared<Coroutine::DispatcherExecutor>(dispatcher)), schema_(schema),
      data_injector_(std::move(data_injector)) {
  for (size_t i = 0; i < filters_.size(); ++i) {
    handoffs_.push_back(
        std::make_shared<Coroutine::AsyncQueue<std::unique_ptr<AiRequest>>>(/*max_size=*/1));
    session_handoffs_.push_back(
        std::make_shared<Coroutine::AsyncQueue<std::unique_ptr<FieldStreamingSession>>>(
            /*max_size=*/1));
    filter_registered_paths_.push_back({});
  }
}

FilterManager::~FilterManager() {
  if (!destroyed_) {
    onDestroy();
  }
}

void FilterManager::start(absl::AnyInvocable<void(absl::Status)> on_complete) {
  on_complete_ = std::move(on_complete);

  if (filters_.empty()) {
    // 0-filter pass-through: stream directly to data_injector_ via sink
    final_request_ = std::make_unique<AiRequest>(std::move(request_json_), *this, 0);
    startFinalStage();
    return;
  }

  launchFilters();

  // Deliver initial request to filter 0
  auto initial_req = std::make_unique<AiRequest>(std::move(request_json_), *this, 0);
  handoffs_[0]->tryPush(std::move(initial_req));
}

void FilterManager::launchFilters() {
  for (size_t i = 0; i < filters_.size(); ++i) {
    auto handoff = handoffs_[i];
    auto session_handoff = session_handoffs_[i];

    AiRequestGetter getter =
        [handoff]() -> Coroutine::Task<absl::StatusOr<std::unique_ptr<AiRequest>>> {
      auto res = co_await handoff->pop();
      if (!res.ok()) {
        co_return res.status();
      }
      if (!res->has_value()) {
        co_return absl::InternalError("request handoff queue empty");
      }
      co_return std::move(**res);
    };

    AiRequestForwarder forwarder = [this, i, session_handoff](std::unique_ptr<AiRequest> req)
        -> Coroutine::Task<absl::StatusOr<std::unique_ptr<FieldStreamingSession>>> {
      if (req == nullptr) {
        co_return absl::InvalidArgumentError("forwarded null AiRequest");
      }

      std::vector<std::string> registered_paths = req->registeredPaths();
      req->clearStreamInterests();
      req->setFilterIndex(i + 1);

      if (i + 1 < filters_.size()) {
        if (registered_paths.empty() && stage_start_index_ == i) {
          // Filter i has no fields to stream and no earlier filter is waiting in this stage.
          stage_start_index_ = i + 1;
          handoffs_[i + 1]->tryPush(std::move(req));
          co_return std::make_unique<FieldStreamingSession>(*this, i, std::vector<std::string>{});
        }

        filter_registered_paths_[i] = std::move(registered_paths);
        handoffs_[i + 1]->tryPush(std::move(req));
        auto session_or = co_await session_handoff->pop();
        if (!session_or.ok()) {
          co_return session_or.status();
        }
        if (!session_or->has_value()) {
          co_return absl::InternalError("session handoff closed unexpectedly");
        }
        co_return std::move(**session_or);
      } else {
        // Last filter in chain
        if (registered_paths.empty() && stage_start_index_ == i) {
          stage_start_index_ = i + 1;
          final_request_ = std::move(req);
          startFinalStage();
          co_return std::make_unique<FieldStreamingSession>(*this, i, std::vector<std::string>{});
        }

        filter_registered_paths_[i] = std::move(registered_paths);
        final_request_ = std::move(req);
        startFinalStage();
        auto session_or = co_await session_handoff->pop();
        if (!session_or.ok()) {
          co_return session_or.status();
        }
        if (!session_or->has_value()) {
          co_return absl::InternalError("session handoff closed unexpectedly");
        }
        co_return std::move(**session_or);
      }
    };

    auto task = filters_[i]->decode(std::move(getter), std::move(forwarder));
    auto handle = Coroutine::launch(
        std::move(task), executor_,
        [this](absl::Status status) {
          if (!status.ok()) {
            onFilterError(std::move(status));
          }
        },
        Coroutine::StartMode::Inline);
    handles_.push_back(std::move(handle));
  }
}

std::unique_ptr<FieldStreamingSession>
FilterManager::startStageStreaming(size_t to_filter_index, const JsonWithExtBuf& doc,
                                   std::vector<std::string> paths) {
  size_t from_filter_index = stage_start_index_;
  stage_start_index_ = to_filter_index;

  // Unblock intermediate filters in this stage
  for (size_t j = from_filter_index; j < to_filter_index; ++j) {
    auto session =
        std::make_unique<FieldStreamingSession>(*this, j, std::move(filter_registered_paths_[j]),
                                                /*is_stage_terminator=*/false);
    filter_registered_paths_[j].clear();
    session_handoffs_[j]->tryPush(std::move(session));
  }

  // Create stage terminator streaming session for to_filter_index
  auto terminator_session = std::make_unique<FieldStreamingSession>(*this, to_filter_index, paths,
                                                                    /*is_stage_terminator=*/true);

  // Determine paths to stream for this stage
  std::vector<std::string> paths_to_stream;
  if (schema_ != nullptr && !schema_->requestStreamableFieldOrder().empty()) {
    for (const auto& path : schema_->requestStreamableFieldOrder()) {
      if (field_dependencies_.contains(path)) {
        paths_to_stream.push_back(path);
      }
    }
    for (const auto& [path, _] : field_dependencies_) {
      if (std::find(paths_to_stream.begin(), paths_to_stream.end(), path) ==
          paths_to_stream.end()) {
        paths_to_stream.push_back(path);
      }
    }
  } else {
    for (const auto& [path, _] : field_dependencies_) {
      paths_to_stream.push_back(path);
    }
  }

  auto source_task = sourceStage(from_filter_index, to_filter_index, doc,
                                 std::move(paths_to_stream), /*is_final=*/false);
  auto handle = Coroutine::launch(
      std::move(source_task), executor_,
      [this](absl::Status status) {
        if (!status.ok()) {
          onFilterError(std::move(status));
        }
      },
      Coroutine::StartMode::Inline);
  handles_.push_back(std::move(handle));

  return terminator_session;
}

void FilterManager::startFinalStage() {
  if (final_stage_started_) {
    return;
  }
  final_stage_started_ = true;

  size_t from_filter_index = stage_start_index_;
  size_t to_filter_index = filters_.empty() ? 0 : filters_.size() - 1;

  if (!filters_.empty() && from_filter_index <= to_filter_index) {
    for (size_t j = from_filter_index; j <= to_filter_index; ++j) {
      auto session =
          std::make_unique<FieldStreamingSession>(*this, j, std::move(filter_registered_paths_[j]),
                                                  /*is_stage_terminator=*/false);
      filter_registered_paths_[j].clear();
      session_handoffs_[j]->tryPush(std::move(session));
    }
  }

  const JsonWithExtBuf& doc = final_request_ != nullptr ? final_request_->doc() : request_json_;

  // Determine field paths to stream in canonical schema order
  std::vector<std::string> paths_to_stream;
  if (schema_ != nullptr && !schema_->requestStreamableFieldOrder().empty()) {
    for (const auto& path : schema_->requestStreamableFieldOrder()) {
      paths_to_stream.push_back(path);
    }
    for (const auto& [path, _] : field_dependencies_) {
      if (std::find(paths_to_stream.begin(), paths_to_stream.end(), path) ==
          paths_to_stream.end()) {
        paths_to_stream.push_back(path);
      }
    }
  } else {
    for (const auto& [path, _] : field_dependencies_) {
      paths_to_stream.push_back(path);
    }
  }

  auto source_task = sourceStage(from_filter_index, to_filter_index, doc,
                                 std::move(paths_to_stream), /*is_final=*/true);
  auto handle = Coroutine::launch(
      std::move(source_task), executor_,
      [this](absl::Status status) {
        if (!status.ok()) {
          onFilterError(std::move(status));
        }
      },
      Coroutine::StartMode::Inline);
  handles_.push_back(std::move(handle));

  startSink();
}

void FilterManager::readFieldToBuffer(const JsonWithExtBuf& doc, const std::string& path,
                                      FieldStream& buf) {
  if (!path.empty() && path[0] == '/') {
    nlohmann::json::json_pointer ptr(path);
    if (doc.json().contains(ptr)) {
      const auto& node = doc.json()[ptr];
      if (JsonWithExtBuf::isExternalRef(node)) {
        auto ref_or = JsonWithExtBuf::externalRef(node);
        if (ref_or.ok() && buffer_manager_ != nullptr) {
          const auto& ref = *ref_or;
          uint64_t remaining = ref.length;
          uint64_t current_offset = ref.offset;
          constexpr uint64_t ChunkSize = 64 * 1024;

          while (remaining > 0) {
            uint64_t to_read = std::min(remaining, ChunkSize);
            bool is_last = (remaining == to_read);
            buffer_manager_->read(
                current_offset, to_read,
                [&buf, is_last](ExternalBufferStatus status, Buffer::InstancePtr data) {
                  if (status == ExternalBufferStatus::Ok && data != nullptr) {
                    Buffer::OwnedImpl owned;
                    owned.move(*data);
                    buf.pushChunk(std::move(owned), is_last);
                  }
                });
            current_offset += to_read;
            remaining -= to_read;
          }
          return;
        }
      } else if (node.is_string()) {
        std::string val = node.get<std::string>();
        Buffer::OwnedImpl owned(val);
        buf.pushChunk(std::move(owned), true);
        return;
      }
    }
  }
  buf.pushChunk(Buffer::OwnedImpl(), true);
}

Coroutine::Task<absl::Status> FilterManager::sourceStage(size_t from_filter_index,
                                                         size_t to_filter_index,
                                                         const JsonWithExtBuf& doc,
                                                         std::vector<std::string> paths_to_stream,
                                                         bool is_final) {
  for (const auto& path : paths_to_stream) {
    auto source_queue = std::make_shared<ByteQueue>();
    FieldStream buf(path, source_queue);
    readFieldToBuffer(doc, path, buf);

    if (is_final) {
      auto& entry = sink_entries_[path];
      if (!entry) {
        entry = std::make_shared<SinkEntry>();
      }
    }

    std::deque<ByteQueuePtr> queues;
    queues.push_back(std::move(source_queue));
    forwardStreamChannel(path, std::move(queues), from_filter_index);
  }

  // Mark sessions done if they have no upstream filters in this stage providing data for their
  // paths
  std::vector<FieldStreamingSession*> sessions_to_mark_done;
  for (const auto& [_, sessions] : field_dependencies_) {
    for (FieldStreamingSession* session : sessions) {
      if (session->filterIndex() >= from_filter_index &&
          session->filterIndex() <= to_filter_index) {
        bool has_upstream = false;
        for (const auto& path : session->registeredPaths()) {
          auto it = field_dependencies_.find(path);
          if (it != field_dependencies_.end()) {
            for (FieldStreamingSession* other : it->second) {
              if (other->filterIndex() >= from_filter_index &&
                  other->filterIndex() < session->filterIndex()) {
                has_upstream = true;
                break;
              }
            }
          }
          if (has_upstream) {
            break;
          }
        }
        if (!has_upstream) {
          sessions_to_mark_done.push_back(session);
        }
      }
    }
  }

  for (FieldStreamingSession* session : sessions_to_mark_done) {
    session->markDone();
  }

  co_return absl::OkStatus();
}

void FilterManager::registerFieldStreamingSession(absl::string_view path,
                                                  FieldStreamingSession* session) {
  field_dependencies_[std::string(path)].push_back(session);
}

void FilterManager::unregisterFieldStreamingSession(absl::string_view path,
                                                    FieldStreamingSession* session) {
  auto it = field_dependencies_.find(path);
  if (it != field_dependencies_.end()) {
    it->second.remove(session);
    if (it->second.empty()) {
      field_dependencies_.erase(it);
    }
  }
}

void FilterManager::markNextStreamingSessionDone(absl::string_view path, size_t from_filter_index) {
  auto it = field_dependencies_.find(path);
  if (it != field_dependencies_.end()) {
    for (FieldStreamingSession* session : it->second) {
      if (session->filterIndex() >= from_filter_index) {
        session->markDone();
        return;
      }
    }
  }

  // Sink:
  if (final_request_ != nullptr) {
    // Final pass sink (after last filter forwarded request): mark dropped
    auto& entry = sink_entries_[std::string(path)];
    if (!entry) {
      entry = std::make_shared<SinkEntry>();
    }
    entry->dropped_ = true;
    entry->queue_.close();
  }
}

void FilterManager::forwardStreamChannel(std::string path, std::deque<ByteQueuePtr> queues,
                                         size_t from_filter_index) {
  auto it = field_dependencies_.find(path);
  if (it != field_dependencies_.end()) {
    for (FieldStreamingSession* session : it->second) {
      if (session->filterIndex() >= from_filter_index) {
        session->insertStreamChannel(std::move(path), std::move(queues));
        return;
      }
    }
  }

  // Sink:
  if (final_request_ != nullptr) {
    // Final pass sink (after last filter forwarded request): deliver to sink entry for
    // serialization
    auto& entry = sink_entries_[path];
    if (!entry) {
      entry = std::make_shared<SinkEntry>();
    }
    FieldStream buf(path, std::move(queues));
    entry->queue_.tryPush(std::move(buf));
  } else {
    // First pass / intermediate sink: drain and discard without serializing
    FieldStream buf(path, std::move(queues));
    buf.drainAll();
  }
}

bool FilterManager::hasFieldStream(absl::string_view path) const {
  return sink_entries_.contains(path);
}

Coroutine::Task<absl::StatusOr<std::optional<FieldStream>>>
FilterManager::getFieldStream(absl::string_view path) {
  auto& entry = sink_entries_[std::string(path)];
  if (!entry) {
    entry = std::make_shared<SinkEntry>();
  }
  if (entry->dropped_) {
    co_return std::nullopt;
  }
  co_return co_await entry->queue_.pop();
}

void FilterManager::startSink() {
  if (sink_started_) {
    return;
  }
  sink_started_ = true;
  auto sink_task = sink();
  auto handle = Coroutine::launch(
      std::move(sink_task), executor_,
      [this](absl::Status status) {
        if (!status.ok()) {
          onFilterError(std::move(status));
          return;
        }
        onFilterChainCompleted();
      },
      Coroutine::StartMode::Inline);
  handles_.push_back(std::move(handle));
}

Coroutine::Task<absl::Status> FilterManager::sink() {
  const JsonWithExtBuf& doc = final_request_ != nullptr ? final_request_->doc() : request_json_;
  co_return co_await Serializer::serialize(
      doc, buffer_manager_,
      [this](Buffer::Instance& chunk, bool end_stream) {
        if (data_injector_) {
          data_injector_(chunk, end_stream);
        }
      },
      this);
}

void FilterManager::onFilterChainCompleted() {
  if (completed_ || destroyed_) {
    return;
  }
  completed_ = true;

  if (on_complete_) {
    on_complete_(absl::OkStatus());
  }
}

void FilterManager::onFilterError(absl::Status status) {
  if (completed_ || destroyed_) {
    return;
  }
  completed_ = true;
  onDestroy();
  if (on_complete_) {
    on_complete_(std::move(status));
  }
}

void FilterManager::onDestroy() {
  if (destroyed_) {
    return;
  }
  destroyed_ = true;
  for (auto& handle : handles_) {
    handle.cancel();
  }
  handles_.clear();
  for (auto& handoff : handoffs_) {
    handoff->close();
  }
  for (auto& session_handoff : session_handoffs_) {
    session_handoff->close();
  }
  for (auto& [_, entry] : sink_entries_) {
    if (entry) {
      entry->queue_.close();
    }
  }
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
