#include "source/extensions/filters/http/ai_protocol_manager/filter_manager.h"

#include <memory>
#include <utility>
#include <vector>

#include "envoy/event/dispatcher.h"
#include "envoy/http/header_map.h"
#include "envoy/stream_info/stream_info.h"

#include "source/common/common/assert.h"
#include "source/extensions/filters/http/ai_protocol_manager/chain_writer.h"
#include "source/extensions/filters/http/ai_protocol_manager/filter_chain_bridge.h"
#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"
#include "source/extensions/filters/http/ai_protocol_manager/payload_reader.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

FilterManager::FilterManager(std::vector<AiFilterSharedPtr> filters)
    : filters_(std::move(filters)) {}

FilterManager::~FilterManager() { cancel(); }

void FilterManager::startRequest(JsonWithExtBuf payload_index, ChainWriter* writer,
                                 PayloadReader* payload_reader, Event::Dispatcher& dispatcher,
                                 StreamInfo::StreamInfo& stream_info, OnCompleteFn on_complete,
                                 Http::RequestHeaderMap* request_headers,
                                 LocalReplyFn local_reply_fn, bool always_serialize,
                                 LLMProtocol request_protocol) {
  ASSERT(request_manager_ == nullptr);
  request_manager_ = std::make_unique<RequestFilterManager>(
      filters_, std::move(payload_index), writer, payload_reader, dispatcher, stream_info,
      std::move(on_complete), request_headers, std::move(local_reply_fn), always_serialize,
      request_protocol);
  request_manager_->start();
}

void FilterManager::startSseResponse(ExternalBufferFactory& buffer_factory,
                                     FilterChainBridge& bridge, ChainWriter& writer,
                                     OnCompleteFn on_complete,
                                     ResponseFilterManager::Config config) {
  ASSERT(response_manager_ == nullptr);
  std::vector<AiFilterSharedPtr> reversed(filters_.rbegin(), filters_.rend());
  response_manager_ = std::make_unique<ResponseFilterManager>(
      std::move(reversed), buffer_factory, bridge, writer, std::move(on_complete), config);
  response_manager_->start();
}

void FilterManager::startUnaryResponse(ExternalBufferFactory& buffer_factory,
                                       FilterChainBridge& bridge, ChainWriter& writer,
                                       OnCompleteFn on_complete) {
  ASSERT(response_manager_ == nullptr);
  std::vector<AiFilterSharedPtr> reversed(filters_.rbegin(), filters_.rend());
  ResponseFilterManager::Config config;
  config.mode = ResponseFilterManager::Mode::Unary;
  response_manager_ = std::make_unique<ResponseFilterManager>(
      std::move(reversed), buffer_factory, bridge, writer, std::move(on_complete), config);
  response_manager_->start();
}

void FilterManager::onResponseData(Buffer::Instance& data, bool end_stream) {
  ASSERT(response_manager_ != nullptr);
  response_manager_->onData(data, end_stream);
}

void FilterManager::cancel() {
  if (request_manager_ != nullptr) {
    request_manager_->cancel();
  }
  if (response_manager_ != nullptr) {
    response_manager_->cancel();
  }
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
