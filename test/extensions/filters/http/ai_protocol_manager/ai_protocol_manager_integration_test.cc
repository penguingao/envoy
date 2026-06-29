#include "envoy/extensions/filters/http/ai_protocol_manager/v3/ai_protocol_manager.pb.h"

#include "test/integration/http_protocol_integration.h"
#include "test/mocks/http/mocks.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace {

class AiProtocolManagerIntegrationTest : public HttpProtocolIntegrationTest {
public:
  void initializeFilter() {
    config_helper_.prependFilter(R"EOF(
      name: envoy.filters.http.ai_protocol_manager
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager
    )EOF");
    initialize();
  }
};

INSTANTIATE_TEST_SUITE_P(
    Protocols, AiProtocolManagerIntegrationTest,
    testing::ValuesIn(HttpProtocolIntegrationTest::getProtocolTestParams()),
    HttpProtocolIntegrationTest::protocolTestParamsToString);

TEST_P(AiProtocolManagerIntegrationTest, HeaderOnlyRequestAndResponse) {
  initializeFilter();
  codec_client_ = makeHttpConnection(lookupPort("http"));
  auto response = codec_client_->makeHeaderOnlyRequest(default_request_headers_);
  waitForNextUpstreamRequest();
  upstream_request_->encodeHeaders(default_response_headers_, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

TEST_P(AiProtocolManagerIntegrationTest, RequestAndResponseWithSmallBody) {
  initializeFilter();
  codec_client_ = makeHttpConnection(lookupPort("http"));
  auto response = codec_client_->makeRequestWithBody(default_request_headers_, 512);
  waitForNextUpstreamRequest();
  upstream_request_->encodeHeaders(default_response_headers_, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
  EXPECT_EQ(512, upstream_request_->bodyLength());
}

TEST_P(AiProtocolManagerIntegrationTest, RequestAndResponseWithLargeBody) {
  initializeFilter();
  codec_client_ = makeHttpConnection(lookupPort("http"));
  // 2500 bytes is larger than the 1024 chunk size, so it will be split.
  auto response = codec_client_->makeRequestWithBody(default_request_headers_, 2500);
  waitForNextUpstreamRequest();
  upstream_request_->encodeHeaders(default_response_headers_, true);
  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
  EXPECT_EQ(2500, upstream_request_->bodyLength());
}

TEST_P(AiProtocolManagerIntegrationTest, RequestWithTrailers) {
  initializeFilter();
  codec_client_ = makeHttpConnection(lookupPort("http"));

  auto encoder_decoder = codec_client_->startRequest(default_request_headers_);
  request_encoder_ = &encoder_decoder.first;
  auto response = std::move(encoder_decoder.second);

  codec_client_->sendData(*request_encoder_, "hello", false);
  Http::TestRequestTrailerMapImpl request_trailers{{"request", "trailer"}};
  codec_client_->sendTrailers(*request_encoder_, request_trailers);

  waitForNextUpstreamRequest();
  upstream_request_->encodeHeaders(default_response_headers_, true);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());

  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ("hello", upstream_request_->body().toString());
  if (upstream_request_->trailers() != nullptr) {
    EXPECT_THAT(*upstream_request_->trailers(), HeaderMapEqualRef(&request_trailers));
  }
}

} // namespace
} // namespace Envoy
