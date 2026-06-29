#include "source/extensions/filters/http/ai_protocol_manager/ai_protocol_manager.h"

#include "test/mocks/buffer/mocks.h"
#include "test/mocks/http/mocks.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

class AiProtocolManagerFilterTest : public testing::Test {
public:
  AiProtocolManagerFilterTest() {
    ON_CALL(decoder_callbacks_, addUpstreamWatermarkCallbacks(_))
        .WillByDefault(Invoke(
            [this](Http::UpstreamWatermarkCallbacks& cb) { upstream_watermark_callbacks_ = &cb; }));
    ON_CALL(decoder_callbacks_, removeUpstreamWatermarkCallbacks(_))
        .WillByDefault(Invoke([this](Http::UpstreamWatermarkCallbacks& cb) {
          if (upstream_watermark_callbacks_ == &cb) {
            upstream_watermark_callbacks_ = nullptr;
          }
        }));
    filter_.setDecoderFilterCallbacks(decoder_callbacks_);
    filter_.setEncoderFilterCallbacks(encoder_callbacks_);
  }

  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_;
  NiceMock<Http::MockStreamEncoderFilterCallbacks> encoder_callbacks_;
  Http::UpstreamWatermarkCallbacks* upstream_watermark_callbacks_{nullptr};
  AiProtocolManagerFilter filter_;
};

TEST_F(AiProtocolManagerFilterTest, BufferSingleChunkAndInject) {
  Http::TestRequestHeaderMapImpl request_headers;
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_.decodeHeaders(request_headers, false));

  Buffer::OwnedImpl request_data("hello world");

  // We expect:
  // 1. High watermark triggered when write starts (pauses client).
  // 2. Low watermark triggered when write completes (resumes client).
  // 3. Data injected.
  InSequence s;
  EXPECT_CALL(decoder_callbacks_, onDecoderFilterAboveWriteBufferHighWatermark());
  EXPECT_CALL(decoder_callbacks_, onDecoderFilterBelowWriteBufferLowWatermark());
  EXPECT_CALL(decoder_callbacks_,
              injectDecodedDataToFilterChain(BufferString("hello world"), true));

  EXPECT_EQ(Http::FilterDataStatus::StopIterationNoBuffer, filter_.decodeData(request_data, true));
}

TEST_F(AiProtocolManagerFilterTest, BufferMultipleChunksAndInject) {
  Http::TestRequestHeaderMapImpl request_headers;
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_.decodeHeaders(request_headers, false));

  // Chunk 1:
  Buffer::OwnedImpl chunk1("hello ");
  {
    InSequence s;
    EXPECT_CALL(decoder_callbacks_, onDecoderFilterAboveWriteBufferHighWatermark());
    EXPECT_CALL(decoder_callbacks_, onDecoderFilterBelowWriteBufferLowWatermark());
  }
  EXPECT_EQ(Http::FilterDataStatus::StopIterationNoBuffer, filter_.decodeData(chunk1, false));

  // Chunk 2:
  Buffer::OwnedImpl chunk2("world");
  {
    InSequence s;
    EXPECT_CALL(decoder_callbacks_, onDecoderFilterAboveWriteBufferHighWatermark());
    EXPECT_CALL(decoder_callbacks_, onDecoderFilterBelowWriteBufferLowWatermark());
    EXPECT_CALL(decoder_callbacks_,
                injectDecodedDataToFilterChain(BufferString("hello world"), true));
  }
  EXPECT_EQ(Http::FilterDataStatus::StopIterationNoBuffer, filter_.decodeData(chunk2, true));
}

TEST_F(AiProtocolManagerFilterTest, BufferLargePayloadAndInjectChunks) {
  Http::TestRequestHeaderMapImpl request_headers;
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_.decodeHeaders(request_headers, false));

  std::string large_payload(2500, 'a');
  Buffer::OwnedImpl request_data(large_payload);

  InSequence s;
  EXPECT_CALL(decoder_callbacks_, onDecoderFilterAboveWriteBufferHighWatermark());
  EXPECT_CALL(decoder_callbacks_, onDecoderFilterBelowWriteBufferLowWatermark());
  EXPECT_CALL(decoder_callbacks_,
              injectDecodedDataToFilterChain(BufferString(std::string(1024, 'a')), false));
  EXPECT_CALL(decoder_callbacks_,
              injectDecodedDataToFilterChain(BufferString(std::string(1024, 'a')), false));
  EXPECT_CALL(decoder_callbacks_,
              injectDecodedDataToFilterChain(BufferString(std::string(452, 'a')), true));

  EXPECT_EQ(Http::FilterDataStatus::StopIterationNoBuffer, filter_.decodeData(request_data, true));
}

TEST_F(AiProtocolManagerFilterTest, DownstreamBackpressurePausesAndResumes) {
  Http::TestRequestHeaderMapImpl request_headers;
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_.decodeHeaders(request_headers, false));

  std::string large_payload(2500, 'a');
  Buffer::OwnedImpl request_data(large_payload);

  InSequence s;
  // Flow control for client during write:
  EXPECT_CALL(decoder_callbacks_, onDecoderFilterAboveWriteBufferHighWatermark());
  EXPECT_CALL(decoder_callbacks_, onDecoderFilterBelowWriteBufferLowWatermark());

  // First chunk injection, during which we simulate next filters becoming backed up:
  EXPECT_CALL(decoder_callbacks_,
              injectDecodedDataToFilterChain(BufferString(std::string(1024, 'a')), false))
      .WillOnce(Invoke([this](Buffer::Instance&, bool) {
        ASSERT_NE(upstream_watermark_callbacks_, nullptr);
        upstream_watermark_callbacks_->onAboveWriteBufferHighWatermark();
      }));

  // Trigger decodeData. It should pause after the first chunk.
  EXPECT_EQ(Http::FilterDataStatus::StopIterationNoBuffer, filter_.decodeData(request_data, true));

  // Now resume, and expect the remaining chunks.
  EXPECT_CALL(decoder_callbacks_,
              injectDecodedDataToFilterChain(BufferString(std::string(1024, 'a')), false));
  EXPECT_CALL(decoder_callbacks_,
              injectDecodedDataToFilterChain(BufferString(std::string(452, 'a')), true));

  ASSERT_NE(upstream_watermark_callbacks_, nullptr);
  upstream_watermark_callbacks_->onBelowWriteBufferLowWatermark();
}

TEST_F(AiProtocolManagerFilterTest, BackpressurePresentBeforeReadBack) {
  Http::TestRequestHeaderMapImpl request_headers;
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_.decodeHeaders(request_headers, false));

  Buffer::OwnedImpl request_data("hello world");

  // Simulate next filters backed up.
  ASSERT_NE(upstream_watermark_callbacks_, nullptr);
  upstream_watermark_callbacks_->onAboveWriteBufferHighWatermark();

  {
    InSequence s;
    // Flow control for client during write:
    EXPECT_CALL(decoder_callbacks_, onDecoderFilterAboveWriteBufferHighWatermark());
    EXPECT_CALL(decoder_callbacks_, onDecoderFilterBelowWriteBufferLowWatermark());
  }

  // decodeData should NOT trigger injection.
  EXPECT_EQ(Http::FilterDataStatus::StopIterationNoBuffer, filter_.decodeData(request_data, true));

  // Now resume, and expect the injection.
  EXPECT_CALL(decoder_callbacks_,
              injectDecodedDataToFilterChain(BufferString("hello world"), true));
  ASSERT_NE(upstream_watermark_callbacks_, nullptr);
  upstream_watermark_callbacks_->onBelowWriteBufferLowWatermark();
}

TEST_F(AiProtocolManagerFilterTest, EmptyPayloadInjectsEmpty) {
  Http::TestRequestHeaderMapImpl request_headers;
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_.decodeHeaders(request_headers, false));

  Buffer::OwnedImpl request_data("");

  EXPECT_CALL(decoder_callbacks_, injectDecodedDataToFilterChain(BufferString(""), true));

  EXPECT_EQ(Http::FilterDataStatus::StopIterationNoBuffer, filter_.decodeData(request_data, true));
}

TEST_F(AiProtocolManagerFilterTest, PassThroughEncodes) {
  Http::TestResponseHeaderMapImpl response_headers;
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_.encodeHeaders(response_headers, false));

  Buffer::OwnedImpl response_data("response_data");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_.encodeData(response_data, false));

  Http::TestResponseTrailerMapImpl response_trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_.encodeTrailers(response_trailers));

  Http::MetadataMap metadata_map;
  EXPECT_EQ(Http::FilterMetadataStatus::Continue, filter_.encodeMetadata(metadata_map));
}

TEST_F(AiProtocolManagerFilterTest, RequestWithTrailers) {
  Http::TestRequestHeaderMapImpl request_headers;
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            filter_.decodeHeaders(request_headers, false));

  // We expect:
  // 1. High watermark triggered when write starts.
  // 2. Low watermark triggered when write completes.
  // 3. Data injected (with end_stream = false).
  // 4. continueDecoding() called because we have trailers.
  InSequence s;
  EXPECT_CALL(decoder_callbacks_, onDecoderFilterAboveWriteBufferHighWatermark());
  EXPECT_CALL(decoder_callbacks_, onDecoderFilterBelowWriteBufferLowWatermark());
  EXPECT_CALL(decoder_callbacks_, injectDecodedDataToFilterChain(BufferString("hello"), false));
  EXPECT_CALL(decoder_callbacks_, continueDecoding());

  Buffer::OwnedImpl request_data("hello");
  EXPECT_EQ(Http::FilterDataStatus::StopIterationNoBuffer, filter_.decodeData(request_data, false));

  Http::TestRequestTrailerMapImpl request_trailers;
  EXPECT_EQ(Http::FilterTrailersStatus::StopIteration, filter_.decodeTrailers(request_trailers));
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
