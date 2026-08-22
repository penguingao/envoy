#pragma once

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "source/common/common/logger.h"
#include "source/common/coroutine/async_queue.h"
#include "source/common/coroutine/dispatcher_executor.h"
#include "source/common/coroutine/launch.h"
#include "source/common/coroutine/task.h"
#include "source/extensions/filters/http/ai_protocol_manager/ai_filter.h"
#include "source/extensions/filters/http/ai_protocol_manager/ai_request.h"
#include "source/extensions/filters/http/ai_protocol_manager/buffer_manager.h"
#include "source/extensions/filters/http/ai_protocol_manager/field_stream.h"
#include "source/extensions/filters/http/ai_protocol_manager/field_streaming_session.h"
#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"
#include "source/extensions/filters/http/ai_protocol_manager/schema.h"
#include "source/extensions/filters/http/ai_protocol_manager/serializer.h"

#include "absl/container/flat_hash_map.h"
#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// FilterManager orchestrates the AI Filter Chain execution for a single stream.
// It manages coroutine lifetimes, request forwarding between filters, staged field streaming,
// and sink serialization.
class FilterManager : public SinkProvider, public Logger::Loggable<Logger::Id::filter> {
public:
  struct SinkEntry {
    Coroutine::AsyncQueue<FieldStream> queue_{1};
    bool dropped_{false};
  };
  using DataInjector = absl::AnyInvocable<void(Buffer::Instance&, bool)>;

  FilterManager(std::vector<AiFilterPtr> filters, JsonWithExtBuf request_json,
                BufferManager* buffer_manager, Event::Dispatcher& dispatcher,
                const PayloadSchema* schema = nullptr, DataInjector data_injector = nullptr);
  ~FilterManager() override;

  // Launches the filter chain. Invokes on_complete with the final completion status.
  void start(absl::AnyInvocable<void(absl::Status)> on_complete);

  // Detaches and cancels all in-flight filter chain coroutines.
  void onDestroy();

  // Dependency registration from FieldStreamingSession
  void registerFieldStreamingSession(absl::string_view path, FieldStreamingSession* session);
  void unregisterFieldStreamingSession(absl::string_view path, FieldStreamingSession* session);

  // Starts sourcing for a filter stage ending at to_filter_index (from `AiRequest::stream()`).
  std::unique_ptr<FieldStreamingSession> startStageStreaming(size_t to_filter_index,
                                                             const JsonWithExtBuf& doc,
                                                             std::vector<std::string> paths);

  // Starts final streaming stage and sink serialization.
  void startFinalStage();

  // Forwards a stream channel (queues) from a filter to the next filter in dependencies (or sink).
  void forwardStreamChannel(std::string path, std::deque<ByteQueuePtr> queues,
                            size_t from_filter_index);

  // Marks the next downstream filter streaming session for the given path done.
  void markNextStreamingSessionDone(absl::string_view path, size_t from_filter_index);

  // SinkProvider implementation
  bool hasFieldStream(absl::string_view path) const override;
  Coroutine::Task<absl::StatusOr<std::optional<FieldStream>>>
  getFieldStream(absl::string_view path) override;

  // Event::Dispatcher access
  Event::Dispatcher& dispatcher() { return dispatcher_; }

  // Returns the current or final JSON document.
  const JsonWithExtBuf& requestJson() const {
    return final_request_ != nullptr ? final_request_->doc() : request_json_;
  }

private:
  // Starts each filter's decode() coroutine.
  void launchFilters();

  // Handoff channels between filters
  std::vector<std::shared_ptr<Coroutine::AsyncQueue<std::unique_ptr<AiRequest>>>> handoffs_;
  std::vector<std::shared_ptr<Coroutine::AsyncQueue<std::unique_ptr<FieldStreamingSession>>>>
      session_handoffs_;
  std::vector<std::vector<std::string>> filter_registered_paths_;
  size_t stage_start_index_{0};

  // Source task for a stage
  Coroutine::Task<absl::Status> sourceStage(size_t from_filter_index, size_t to_filter_index,
                                            const JsonWithExtBuf& doc,
                                            std::vector<std::string> paths_to_stream,
                                            bool is_final);
  void readFieldToBuffer(const JsonWithExtBuf& doc, const std::string& path, FieldStream& buf);

  // Sink task: streams modified buffers and serializes JSON payload directly to data_injector_.
  void startSink();
  Coroutine::Task<absl::Status> sink();

  void onFilterChainCompleted();
  void onFilterError(absl::Status status);

  std::vector<AiFilterPtr> filters_;
  JsonWithExtBuf request_json_;
  std::unique_ptr<AiRequest> final_request_;
  BufferManager* buffer_manager_{nullptr};
  Event::Dispatcher& dispatcher_;
  std::shared_ptr<Coroutine::DispatcherExecutor> executor_;
  const PayloadSchema* schema_{nullptr};
  DataInjector data_injector_{nullptr};

  absl::flat_hash_map<std::string, std::list<FieldStreamingSession*>> field_dependencies_;
  std::vector<Coroutine::DetachedHandle> handles_;
  absl::AnyInvocable<void(absl::Status)> on_complete_;

  absl::flat_hash_map<std::string, std::shared_ptr<SinkEntry>> sink_entries_;

  bool final_stage_started_{false};
  bool sink_started_{false};
  bool completed_{false};
  bool destroyed_{false};
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
