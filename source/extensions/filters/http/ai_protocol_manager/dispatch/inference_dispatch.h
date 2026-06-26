#pragma once

#include "envoy/http/filter.h"

#include "source/common/common/logger.h"
#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_request.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Dispatch {

// InferenceDispatch — chain-forward tail for OpenAI-style inference requests.
//
// Called at the end of the inference chain request-side phases (after Q1 and
// Q2) to re-encode the (possibly mutated) AiRequest with InferencePayload back
// into an OpenAI-compatible HTTP request that downstream Envoy filters (e.g.
// the router filter) can deliver to the upstream model provider.
//
// Dispatch sequence (DESIGN.md §6.3, chain-forward mode):
//
//   1. RequestEncoder::encodeInferenceBody(request)
//        → OpenAI JSON body string reflecting any chain-filter mutations to
//          model name, sampling params, messages[], and tools[].
//          Returns "" for bodiless invocations (GET /v1/responses/{id}, …).
//
//   2. Mutate the downstream request header map in place via
//      callbacks.requestHeaders():
//        :method        ← request.http_method  (POST for bodied creates, GET/DELETE otherwise)
//        :path          ← request.path         (may be rewritten by routing chain filters)
//        content-type   ← "application/json"   (bodied requests only)
//        content-length ← body byte count      (bodied requests only)
//
//   3. For bodied requests: inject the re-encoded body buffer into the filter
//      manager via callbacks.addDecodedData(). The filter manager replays this
//      buffer to subsequent decoder filters and the router when
//      continueDecoding() resumes the decode chain.
//      Bodiless requests skip steps 3 and the content-type / content-length
//      header mutations — headers are already correct from the original request.
//
//   4. callbacks.continueDecoding() — resumes the Envoy decode chain at the
//      filter after AiProtocolManagerFilter.
class InferenceDispatch : public Logger::Loggable<Logger::Id::filter> {
public:
  // Performs the full chain-forward dispatch for an inference request.
  // `request` must have protocol == Inference and a populated InferencePayload.
  // This method calls callbacks.continueDecoding() internally.
  static void dispatch(Codec::AiRequest& request,
                       Http::StreamDecoderFilterCallbacks& callbacks);
};

} // namespace Dispatch
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
