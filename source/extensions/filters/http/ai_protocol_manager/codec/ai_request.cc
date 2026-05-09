#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"

#include <atomic>
#include <memory>

#include "envoy/event/dispatcher.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

std::string materializeRef(const PayloadRef& ref, const AiRequest& request) {
  if (ref.storage() != PayloadRef::Storage::External) {
    return ref.toString();
  }
  ASSERT(request.payload_store != nullptr);
  std::string result;
  if (request.payload_store != nullptr) {
    request.payload_store->fetch(ref, [&](Buffer::InstancePtr buf) {
      if (buf) {
        result = buf->toString();
      }
    });
  }
  return result;
}

void prefetchExternalRefs(AiRequest& request, Event::Dispatcher& dispatcher,
                          std::function<void()> on_done) {
  if (request.payload_store == nullptr) {
    on_done();
    return;
  }

  std::vector<PayloadRef*> external;
  auto collect = [&](PayloadRef& ref) {
    if (ref.storage() == PayloadRef::Storage::External) {
      external.push_back(&ref);
    }
  };

  if (auto* p = request.as_inference()) {
    for (auto& ref : p->messages)     collect(ref);
    for (auto& ref : p->tools)        collect(ref);
    for (auto& ref : p->attachments)  collect(ref);
    collect(p->residual_params);
  } else if (auto* p = request.as_agent()) {
    for (auto& ref : p->parts) collect(ref);
    collect(p->arguments);
    collect(p->capabilities);
    collect(p->params_raw);
    collect(p->residual_params);
  }

  if (external.empty()) {
    on_done();
    return;
  }

  auto remaining   = std::make_shared<std::atomic<size_t>>(external.size());
  auto shared_done = std::make_shared<std::function<void()>>(std::move(on_done));

  for (PayloadRef* ref : external) {
    request.payload_store->fetchAsync(
        *ref, dispatcher,
        [ref, remaining, shared_done](Buffer::InstancePtr buf) mutable {
          if (buf && buf->length() > 0) {
            *ref = PayloadRef::makeBuffered(std::move(buf));
          }
          if (--(*remaining) == 0) {
            (*shared_done)();
          }
        });
  }
}

InferencePayload* AiRequest::as_inference() {
  return std::get_if<InferencePayload>(&payload);
}

const InferencePayload* AiRequest::as_inference() const {
  return std::get_if<InferencePayload>(&payload);
}

AgentPayload* AiRequest::as_agent() {
  return std::get_if<AgentPayload>(&payload);
}

const AgentPayload* AiRequest::as_agent() const {
  return std::get_if<AgentPayload>(&payload);
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
