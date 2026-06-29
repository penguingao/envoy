#pragma once

#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.h"

#include "source/extensions/filters/http/ai_protocol_manager/external_buffer.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

class AiProtocolManagerFilter : public Http::PassThroughFilter,
                                public Http::UpstreamWatermarkCallbacks {
public:
  AiProtocolManagerFilter();
  ~AiProtocolManagerFilter() override;

  // Http::StreamFilterBase
  void onDestroy() override;

  // Http::StreamDecoderFilter
  Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;
  void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override;

  // Http::UpstreamWatermarkCallbacks
  void onAboveWriteBufferHighWatermark() override;
  void onBelowWriteBufferLowWatermark() override;

private:
  struct WriteCallback : public ExternalBuffer::WriteCallback {
    WriteCallback(AiProtocolManagerFilter& filter, Buffer::InstancePtr chunk)
        : filter_(filter), chunk_(std::move(chunk)) {}
    void onWriteComplete(absl::Status status) override;
    AiProtocolManagerFilter& filter_;
    Buffer::InstancePtr chunk_;
  };

  struct ReadCallback : public ExternalBuffer::ReadCallback {
    ReadCallback(AiProtocolManagerFilter& filter) : filter_(filter) {}
    void onReadComplete(absl::StatusOr<Buffer::InstancePtr> data) override;
    AiProtocolManagerFilter& filter_;
  };

  void onWriteComplete(absl::Status status);
  void onReadComplete(absl::StatusOr<Buffer::InstancePtr> data);

  void startReadingBack();
  void readNextChunk();

  ExternalBufferPtr buffer_;
  std::shared_ptr<bool> destroyed_ = std::make_shared<bool>(false);

  std::shared_ptr<WriteCallback> active_write_callback_;
  std::shared_ptr<ReadCallback> active_read_callback_;

  bool pending_write_ = false;
  bool seen_end_stream_ = false;
  bool upstream_backpressured_ = false;

  uint64_t read_offset_ = 0;
  uint64_t total_size_ = 0;
  const uint64_t chunk_size_ = 1024; // 1KB chunks for streaming back
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
