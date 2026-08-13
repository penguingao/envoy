#pragma once

#include <memory>
#include <string>

#include "envoy/common/pure.h"
#include "envoy/config/typed_config.h"
#include "envoy/event/dispatcher.h"
#include "envoy/http/header_map.h"
#include "envoy/server/factory_context.h"
#include "envoy/stream_info/stream_info.h"

#include "source/common/coroutine/task.h"
#include "source/extensions/filters/http/ai_protocol_manager/inference_request.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace Envoy {
namespace Extensions {
namespace AiFilters {

using HttpFilters::AiProtocolManager::InferenceRequest;
using HttpFilters::AiProtocolManager::InferenceRequestPtr;

// What happens to the request once decode() returns.
enum class PostDecodeAction {
  // Done: the filter wants nothing further from this request.
  Skip,
  // Terminate the request.
  Reset,
};

// The host's surface, granted to each filter for the life of the stream.
//
// Deliberately not the HTTP filter callbacks: an AI filter acts on the payload,
// and the few HTTP-level things it legitimately needs are named here so the set
// stays small and easy to review.
class AiFilterCallbacks {
public:
  virtual ~AiFilterCallbacks() = default;

  // The request headers, still held by the host and so still ahead of routing:
  // a header set here is visible to route selection, which is what lets a filter
  // route on something it found in the payload.
  virtual Http::RequestHeaderMapOptRef requestHeaders() PURE;

  // Terminates the request with a local reply. `details` lands in
  // response_code_details.
  virtual void sendLocalReply(Http::Code code, absl::string_view body,
                              absl::string_view details) PURE;

  virtual StreamInfo::StreamInfo& streamInfo() PURE;

  virtual Event::Dispatcher& dispatcher() PURE;
};

// Hands the filter the parsed request.
//
// Awaiting this is what suspends a filter until the request reaches it, so the
// chain hands the payload along one filter at a time rather than sharing it.
class InferenceRequestGetter {
public:
  virtual ~InferenceRequestGetter() = default;

  // Resolves once the request has reached this filter. Fails if the stream ends
  // first.
  virtual Coroutine::Task<absl::StatusOr<InferenceRequestPtr>> get() PURE;
};

// Passes the request on.
//
// A filter that returns without forwarding does not strand the request -- the
// host forwards it on the filter's behalf -- so forwarding is about *when*, not
// whether. Forward early to let the rest of the chain work while this filter
// keeps going; hold it to be sure nothing downstream sees a payload this filter
// has not finished with.
class InferenceRequestForwarder {
public:
  virtual ~InferenceRequestForwarder() = default;

  // Hands `request` to the next filter, or to the host if this is the last one.
  virtual Coroutine::Task<absl::Status> forward(InferenceRequestPtr request) PURE;
};

// An AI filter: one step in the chain the AI protocol manager runs over a parsed
// payload.
//
// Written as a coroutine rather than a callback state machine because the work
// is inherently sequential -- wait for the request, inspect it, forward it,
// maybe wait again -- and that reads as straight-line code only if it can
// suspend.
class AiFilter {
public:
  virtual ~AiFilter() = default;

  virtual void setCallbacks(AiFilterCallbacks& callbacks) PURE;

  // Runs this filter over the request. `getter` and `forwarder` are owned by the
  // host and outlive the coroutine.
  virtual Coroutine::Task<absl::StatusOr<PostDecodeAction>>
  decode(InferenceRequestGetter& getter, InferenceRequestForwarder& forwarder) PURE;

  // The stream is going away. The coroutine is cancelled separately; this is for
  // releasing anything the filter owns.
  virtual void onDestroy() PURE;
};
using AiFilterPtr = std::unique_ptr<AiFilter>;

// Sink the host offers a factory when building a chain.
class AiFilterChainFactoryCallbacks {
public:
  virtual ~AiFilterChainFactoryCallbacks() = default;
  virtual void addFilter(AiFilterPtr filter) PURE;
};

using AiFilterFactoryCb = std::function<void(AiFilterChainFactoryCallbacks&)>;

// Config factory for an AI filter. Registered under the `envoy.filters.ai`
// category and looked up by the AI protocol manager from the type URL in its
// configured chain.
class NamedAiFilterConfigFactory : public Config::TypedFactory {
public:
  // `context` is a ServerFactoryContext rather than a FactoryContext because the
  // host filter is registered on both the downstream and upstream HTTP chains,
  // and that is all it has to offer on both.
  virtual absl::StatusOr<AiFilterFactoryCb>
  createFilterFactoryFromProto(const Protobuf::Message& config, const std::string& stats_prefix,
                               Server::Configuration::ServerFactoryContext& context) PURE;

  std::string category() const override { return "envoy.filters.ai"; }
};

} // namespace AiFilters
} // namespace Extensions
} // namespace Envoy
