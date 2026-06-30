#pragma once
#include "envoy/buffer/buffer.h"
#include "envoy/http/filter.h"

#include "source/common/buffer/watermark_buffer.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer.h"

#include "absl/status/status.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

class BufferManager : public ExternalBuffer::WriteCallback,
                      public ExternalBuffer::ReadCallback,
                      public Http::UpstreamWatermarkCallbacks {
public:
  class Callbacks {
  public:
    virtual ~Callbacks() = default;

    /**
     * Pauses the source of the data (e.g., downstream for decode, upstream for encode).
     */
    virtual void pauseSource() = 0;

    /**
     * Resumes the source of the data.
     */
    virtual void resumeSource() = 0;

    /**
     * Injects data back into the filter chain.
     */
    virtual void injectData(Buffer::Instance& data, bool end_stream) = 0;

    /**
     * Called when all data has been injected.
     */
    virtual void onDecodingComplete() = 0;

    /**
     * Handles failure (e.g., sends local reply or resets stream).
     */
    virtual void onFailure(absl::Status status) = 0;

    /**
     * @return the dispatcher for posting callbacks.
     */
    virtual Event::Dispatcher& dispatcher() = 0;
  };

  BufferManager(ExternalBufferPtr buffer, Callbacks& callbacks, uint64_t chunk_size,
                uint64_t buffer_limit);
  ~BufferManager() override;

  /**
   * Processes incoming data.
   * @return the filter data status.
   */
  Http::FilterDataStatus onData(Buffer::Instance& data, bool end_stream);

  /**
   * Called when the stream ends (either via data or trailers).
   */
  void setEndStream(bool has_trailers);

  /**
   * Called when the sink (next filters) is backed up.
   */
  void onSinkHighWatermark();

  /**
   * Called when the sink is no longer backed up.
   */
  void onSinkLowWatermark();

  /**
   * Registers watermark callbacks.
   */
  void
  registerWatermarkCallbacks(std::function<void(Http::UpstreamWatermarkCallbacks&)> register_fn,
                             std::function<void(Http::UpstreamWatermarkCallbacks&)> unregister_fn);

  /**
   * Called when the filter is being destroyed.
   */
  void onDestroy();

  // ExternalBuffer::WriteCallback
  void onWriteComplete(absl::Status status) override;

  // ExternalBuffer::ReadCallback
  void onReadComplete(absl::StatusOr<Buffer::InstancePtr> data) override;

  // Http::UpstreamWatermarkCallbacks
  void onAboveWriteBufferHighWatermark() override;
  void onBelowWriteBufferLowWatermark() override;

private:
  void triggerWrite();
  void onWriteCompleteInternal(absl::Status status);
  void onReadCompleteInternal(absl::StatusOr<Buffer::InstancePtr> data);
  void startReadingBack();
  void readNextChunk();

  ExternalBufferPtr buffer_;
  Callbacks& callbacks_;
  const uint64_t chunk_size_;

  Buffer::WatermarkBuffer write_queue_;
  Buffer::InstancePtr active_write_chunk_;

  bool pending_write_ = false;
  bool seen_end_stream_ = false;
  bool has_trailers_ = false;
  bool sink_backed_up_ = false;
  bool reading_back_ = false;

  uint64_t read_offset_ = 0;
  uint64_t total_size_ = 0;

  std::shared_ptr<bool> destroyed_ = std::make_shared<bool>(false);
  std::function<void(Http::UpstreamWatermarkCallbacks&)> unregister_fn_;
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
