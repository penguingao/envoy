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
// When decodeHeaders is called, all preceding filters (ext_proc) have finished
// processing, so the headers reflect the processor's modifications.
class CaptureFilter : public Http::StreamDecoderFilter {
public:
  CaptureFilter(uint32_t index, CaptureCallback callback)
      : index_(index), callback_(std::move(callback)) {}

  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers, bool) override {
    callback_(index_, headers);
    return Http::FilterHeadersStatus::StopIteration;
  }

  Http::FilterDataStatus decodeData(Buffer::Instance&, bool) override {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap&) override {
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
};

} // namespace ParallelExtProc
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
