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
// ext_proc filter has finished processing. The completion callback fires
// exactly once:
// - when wait_for_end_stream=true: on the first call that observes
//   end_stream=true (decodeHeaders, decodeData, or decodeTrailers). This is
//   used in buffered mode so the parent only merges after the processor has
//   seen the full request.
// - when wait_for_end_stream=false: on decodeHeaders regardless of
//   end_stream. This is used in streaming mode so the parent releases the
//   main chain as soon as the processor's header response arrives, letting
//   the body stream through independently.
class CaptureFilter : public Http::StreamDecoderFilter {
public:
  CaptureFilter(uint32_t index, bool wait_for_end_stream, CaptureCallback callback)
      : index_(index), wait_for_end_stream_(wait_for_end_stream),
        callback_(std::move(callback)) {}

  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override {
    headers_ref_ = &headers;
    if (!fired_ && (!wait_for_end_stream_ || end_stream)) {
      fired_ = true;
      callback_(index_, headers);
    }
    return Http::FilterHeadersStatus::StopIteration;
  }

  Http::FilterDataStatus decodeData(Buffer::Instance&, bool end_stream) override {
    if (!fired_ && end_stream && headers_ref_ != nullptr) {
      fired_ = true;
      callback_(index_, *headers_ref_);
    }
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap&) override {
    if (!fired_ && headers_ref_ != nullptr) {
      fired_ = true;
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
  const bool wait_for_end_stream_;
  CaptureCallback callback_;
  Http::StreamDecoderFilterCallbacks* callbacks_{};
  Http::RequestHeaderMap* headers_ref_{};
  bool fired_{false};
};

} // namespace ParallelExtProc
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
