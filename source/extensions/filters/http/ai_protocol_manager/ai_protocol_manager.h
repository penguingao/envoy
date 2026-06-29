#pragma once

#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.h"

#include "source/extensions/filters/http/ai_protocol_manager/buffer_manager.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

class AiProtocolManagerFilter : public Http::PassThroughFilter {
public:
  AiProtocolManagerFilter();
  ~AiProtocolManagerFilter() override;

  // Http::StreamFilterBase
  void onDestroy() override;

  // Http::StreamDecoderFilter
  Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;
  void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override;

private:
  struct DecodeCallbacks : public BufferManager::Callbacks {
    DecodeCallbacks(AiProtocolManagerFilter& filter) : filter_(filter) {}
    void pauseSource() override;
    void resumeSource() override;
    void injectData(Buffer::Instance& data, bool end_stream) override;
    void continueIteration() override;
    void onFailure(absl::Status status) override;
    Event::Dispatcher& dispatcher() override;
    AiProtocolManagerFilter& filter_;
  };

  DecodeCallbacks decode_callbacks_{*this};
  std::unique_ptr<BufferManager> decode_buffer_manager_;
  const uint64_t chunk_size_ = 1024; // 1KB chunks for streaming back
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
