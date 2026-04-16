#pragma once

#include <functional>

#include "envoy/http/filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ParallelExtProc {

// Callback invoked when the terminal CaptureFilter receives modified headers.
using CaptureCallback = std::function<void(uint32_t index, Http::RequestHeaderMap& headers)>;

// Terminal decoder filter placed at the end of each processor's filter chain.
// Captures the modified request headers (and later body/trailers) after the
// ext_proc filter has finished processing. The completion callback fires only
// when end_stream=true is observed, so for requests with a body the callback
// fires when decodeData sees end_stream, guaranteeing the processor has
// already received and acknowledged the body.
class CaptureFilter : public Http::StreamDecoderFilter {
public:
  CaptureFilter(uint32_t index, CaptureCallback callback)
      : index_(index), callback_(std::move(callback)) {}

  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override {
    headers_ref_ = &headers;
    if (end_stream) {
      callback_(index_, headers);
    }
    return Http::FilterHeadersStatus::StopIteration;
  }

  Http::FilterDataStatus decodeData(Buffer::Instance&, bool end_stream) override {
    if (end_stream && headers_ref_ != nullptr) {
      callback_(index_, *headers_ref_);
    }
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap&) override {
    if (headers_ref_ != nullptr) {
      callback_(index_, *headers_ref_);
    }
    return Http::FilterTrailersStatus::StopIteration;
  }

  void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override {
    callbacks_ = &callbacks;
  }

  void onDestroy() override {}

private:
  const uint32_t index_;
  CaptureCallback callback_;
  Http::StreamDecoderFilterCallbacks* callbacks_{};
  Http::RequestHeaderMap* headers_ref_{};
};

} // namespace ParallelExtProc
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
