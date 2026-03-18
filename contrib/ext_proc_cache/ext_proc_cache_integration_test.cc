#include <ctime>
#include <memory>
#include <string>

#include "envoy/extensions/filters/http/ext_proc/v3/ext_proc.pb.h"
#include "envoy/extensions/filters/network/http_connection_manager/v3/http_connection_manager.pb.h"

#include "source/common/protobuf/utility.h"

#include "test/integration/http_integration.h"
#include "test/test_common/utility.h"

#include "contrib/ext_proc_cache/cache_age_calculator.h"
#include "contrib/ext_proc_cache/cache_key_generator.h"
#include "contrib/ext_proc_cache/cache_lookup_coordinator.h"
#include "contrib/ext_proc_cache/cacheability_checker.h"
#include "contrib/ext_proc_cache/in_memory_cache_store.h"
#include "contrib/ext_proc_cache/server.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {
namespace {

class ExtProcCacheIntegrationTest : public HttpIntegrationTest,
                                    public testing::TestWithParam<Network::Address::IpVersion> {
public:
  ExtProcCacheIntegrationTest()
      : HttpIntegrationTest(Http::CodecType::HTTP2, GetParam()) {}

  // Generate an HTTP date string for the current time.
  static std::string httpDateNow() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    gmtime_r(&now_t, &tm_buf);
    char buf[64];
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
    return std::string(buf);
  }

  void SetUp() override {
    // Wire up the ext_proc cache server with default implementations.
    store_ = std::make_shared<InMemoryCacheStore>();
    auto key_gen = std::make_shared<DefaultCacheKeyGenerator>();
    auto cacheability = std::make_shared<DefaultCacheabilityChecker>();
    auto age_calc = std::make_shared<DefaultCacheAgeCalculator>();
    coordinator_ = std::make_shared<CacheLookupCoordinator>(store_);

    // Start the gRPC server on a random port (IPv4).
    cache_server_.start("127.0.0.1:0", coordinator_, key_gen, cacheability, age_calc,
                        chunk_size_);
  }

  void TearDown() override {
    cleanupUpstreamAndDownstream();
    cache_server_.shutdown();
  }

  void initializeWithExtProc() {
    const int cache_server_port = cache_server_.port();

    config_helper_.addConfigModifier(
        [cache_server_port](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
          // Ensure upstream cluster uses HTTP/2.
          ConfigHelper::setHttp2(
              *(bootstrap.mutable_static_resources()->mutable_clusters()->Mutable(0)));

          // Add a cluster for the ext_proc cache gRPC server.
          auto* ext_proc_cluster = bootstrap.mutable_static_resources()->add_clusters();
          ext_proc_cluster->MergeFrom(bootstrap.static_resources().clusters()[0]);
          ext_proc_cluster->set_name("ext_proc_cache_server");
          ext_proc_cluster->mutable_load_assignment()->set_cluster_name("ext_proc_cache_server");
          ConfigHelper::setHttp2(*ext_proc_cluster);

          // Set the address to point at our cache server.
          auto* endpoint = ext_proc_cluster->mutable_load_assignment()
                               ->mutable_endpoints(0)
                               ->mutable_lb_endpoints(0)
                               ->mutable_endpoint()
                               ->mutable_address()
                               ->mutable_socket_address();
          endpoint->set_address("127.0.0.1");
          endpoint->set_port_value(cache_server_port);
        });

    // Build ext_proc filter config using Envoy gRPC with the cluster.
    envoy::extensions::filters::http::ext_proc::v3::ExternalProcessor ext_proc_config;
    ext_proc_config.mutable_grpc_service()->mutable_envoy_grpc()->set_cluster_name(
        "ext_proc_cache_server");
    ext_proc_config.mutable_grpc_service()->mutable_timeout()->CopyFrom(
        Protobuf::util::TimeUtil::SecondsToDuration(5));

    // The cache server requires FULL_DUPLEX_STREAMED mode for the response body.
    // This mode also requires trailer modes to be set to SEND.
    auto* processing_mode = ext_proc_config.mutable_processing_mode();
    processing_mode->set_request_header_mode(
        envoy::extensions::filters::http::ext_proc::v3::ProcessingMode::SEND);
    processing_mode->set_response_header_mode(
        envoy::extensions::filters::http::ext_proc::v3::ProcessingMode::SEND);
    processing_mode->set_request_body_mode(
        envoy::extensions::filters::http::ext_proc::v3::ProcessingMode::NONE);
    processing_mode->set_response_body_mode(
        envoy::extensions::filters::http::ext_proc::v3::ProcessingMode::FULL_DUPLEX_STREAMED);
    processing_mode->set_request_trailer_mode(
        envoy::extensions::filters::http::ext_proc::v3::ProcessingMode::SEND);
    processing_mode->set_response_trailer_mode(
        envoy::extensions::filters::http::ext_proc::v3::ProcessingMode::SEND);

    envoy::extensions::filters::network::http_connection_manager::v3::HttpFilter ext_proc_filter;
    ext_proc_filter.set_name("envoy.filters.http.ext_proc");
    ext_proc_filter.mutable_typed_config()->PackFrom(ext_proc_config);
    config_helper_.prependFilter(MessageUtil::getJsonStringFromMessageOrError(ext_proc_filter));

    setUpstreamProtocol(Http::CodecType::HTTP2);
    setDownstreamProtocol(Http::CodecType::HTTP2);

    HttpIntegrationTest::initialize();
    codec_client_ = makeHttpConnection(makeClientConnection(lookupPort("http")));
  }

  // Helper: send a GET request and get response from upstream (cache miss flow).
  IntegrationStreamDecoderPtr sendRequestGetFromUpstream(
      const Http::TestRequestHeaderMapImpl& request_headers, const std::string& response_body,
      const Http::TestResponseHeaderMapImpl& upstream_response_headers) {
    auto response = codec_client_->makeHeaderOnlyRequest(request_headers);

    EXPECT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_,
                                                           fake_upstream_connection_));
    EXPECT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
    EXPECT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

    upstream_request_->encodeHeaders(upstream_response_headers, false);
    upstream_request_->encodeData(response_body, true);

    EXPECT_TRUE(response->waitForEndStream());
    return response;
  }

  void resetUpstreamConnection() {
    if (fake_upstream_connection_) {
      ASSERT_TRUE(fake_upstream_connection_->close());
      ASSERT_TRUE(fake_upstream_connection_->waitForDisconnect());
      fake_upstream_connection_.reset();
    }
  }

  std::shared_ptr<InMemoryCacheStore> store_;
  std::shared_ptr<CacheLookupCoordinator> coordinator_;
  ExtProcCacheServer cache_server_;
  size_t chunk_size_ = 65536;
};

INSTANTIATE_TEST_SUITE_P(IpVersions, ExtProcCacheIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

// Verifies the basic cache miss -> fill -> cache hit flow.
// First request: cache miss, forwarded to upstream, response cached.
// Second request: cache hit, served directly by ext_proc via ImmediateResponse.
TEST_P(ExtProcCacheIntegrationTest, MissInsertHit) {
  initializeWithExtProc();

  const std::string request_path = "/test/cacheable";
  const std::string response_body = "hello from upstream";
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"},
      {":path", request_path},
      {":scheme", "http"},
      {":authority", "cache-test-host"}};
  const std::string date = httpDateNow();
  Http::TestResponseHeaderMapImpl upstream_response_headers{
      {":status", "200"},
      {"cache-control", "public, max-age=3600"},
      {"date", date},
      {"content-length", std::to_string(response_body.size())}};

  // --- First request: cache miss, response from upstream ---
  {
    auto response =
        sendRequestGetFromUpstream(request_headers, response_body, upstream_response_headers);
    EXPECT_TRUE(response->complete());
    EXPECT_EQ("200", response->headers().getStatusValue());
    EXPECT_EQ(response_body, response->body());
  }

  resetUpstreamConnection();

  // Verify the cache store has the entry.
  EXPECT_EQ(store_->size(), 1);

  // --- Second request: cache hit, served via ImmediateResponse ---
  {
    auto response = codec_client_->makeHeaderOnlyRequest(request_headers);

    // Should NOT reach the upstream — served from cache.
    ASSERT_TRUE(response->waitForEndStream());
    EXPECT_TRUE(response->complete());
    EXPECT_EQ("200", response->headers().getStatusValue());
    EXPECT_EQ(response_body, response->body());

    // Should have an age header since it's served from cache.
    EXPECT_FALSE(response->headers().get(Http::LowerCaseString("age")).empty());
  }
}

// Verifies that non-cacheable requests (POST) bypass the cache.
TEST_P(ExtProcCacheIntegrationTest, NonCacheableRequestBypassesCache) {
  initializeWithExtProc();

  const std::string request_path = "/test/non-cacheable";
  const std::string response_body = "post response";

  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "POST"},
      {":path", request_path},
      {":scheme", "http"},
      {":authority", "cache-test-host"}};

  auto encoder_decoder = codec_client_->startRequest(request_headers);
  auto& encoder = encoder_decoder.first;
  auto response = std::move(encoder_decoder.second);
  codec_client_->sendData(encoder, "post body", true);

  ASSERT_TRUE(
      fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

  Http::TestResponseHeaderMapImpl upstream_response_headers{
      {":status", "200"},
      {"cache-control", "public, max-age=3600"},
      {"date", httpDateNow()},
      {"content-length", std::to_string(response_body.size())}};
  upstream_request_->encodeHeaders(upstream_response_headers, false);
  upstream_request_->encodeData(response_body, true);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
  EXPECT_EQ(response_body, response->body());
}

// Verifies that different paths result in different cache entries.
TEST_P(ExtProcCacheIntegrationTest, DifferentPathsDifferentCacheEntries) {
  initializeWithExtProc();

  const std::string path_a = "/test/path-a";
  const std::string path_b = "/test/path-b";
  const std::string body_a = "response A";
  const std::string body_b = "response B";

  // Request path A -- cache miss.
  {
    Http::TestRequestHeaderMapImpl req{{":method", "GET"},
                                       {":path", path_a},
                                       {":scheme", "http"},
                                       {":authority", "cache-test-host"}};
    Http::TestResponseHeaderMapImpl resp{{":status", "200"},
                                          {"cache-control", "public, max-age=3600"},
                                          {"date", httpDateNow()},
                                          {"content-length", std::to_string(body_a.size())}};
    auto response = sendRequestGetFromUpstream(req, body_a, resp);
    EXPECT_TRUE(response->complete());
    EXPECT_EQ(body_a, response->body());
  }
  resetUpstreamConnection();

  // Request path B -- cache miss (different key).
  {
    Http::TestRequestHeaderMapImpl req{{":method", "GET"},
                                       {":path", path_b},
                                       {":scheme", "http"},
                                       {":authority", "cache-test-host"}};
    Http::TestResponseHeaderMapImpl resp{{":status", "200"},
                                          {"cache-control", "public, max-age=3600"},
                                          {"date", httpDateNow()},
                                          {"content-length", std::to_string(body_b.size())}};
    auto response = sendRequestGetFromUpstream(req, body_b, resp);
    EXPECT_TRUE(response->complete());
    EXPECT_EQ(body_b, response->body());
  }
  resetUpstreamConnection();

  // Request path A again -- cache hit with body_a.
  {
    Http::TestRequestHeaderMapImpl req{{":method", "GET"},
                                       {":path", path_a},
                                       {":scheme", "http"},
                                       {":authority", "cache-test-host"}};
    auto response = codec_client_->makeHeaderOnlyRequest(req);
    ASSERT_TRUE(response->waitForEndStream());
    EXPECT_TRUE(response->complete());
    EXPECT_EQ(body_a, response->body());
    EXPECT_FALSE(response->headers().get(Http::LowerCaseString("age")).empty());
  }

  // Request path B again -- cache hit with body_b.
  {
    Http::TestRequestHeaderMapImpl req{{":method", "GET"},
                                       {":path", path_b},
                                       {":scheme", "http"},
                                       {":authority", "cache-test-host"}};
    auto response = codec_client_->makeHeaderOnlyRequest(req);
    ASSERT_TRUE(response->waitForEndStream());
    EXPECT_TRUE(response->complete());
    EXPECT_EQ(body_b, response->body());
    EXPECT_FALSE(response->headers().get(Http::LowerCaseString("age")).empty());
  }
}

// Verifies that a cache entry with no-cache triggers revalidation with the
// upstream on every request instead of being served from cache.
TEST_P(ExtProcCacheIntegrationTest, StaleCacheEntryRequiresRevalidation) {
  initializeWithExtProc();

  const std::string request_path = "/test/stale";
  const std::string original_body = "original response";
  const std::string revalidated_body = "revalidated response";
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"},
      {":path", request_path},
      {":scheme", "http"},
      {":authority", "cache-test-host"}};

  // --- First request: cache miss, store with no-cache (always requires revalidation) ---
  {
    Http::TestResponseHeaderMapImpl upstream_response_headers{
        {":status", "200"},
        {"cache-control", "no-cache"},
        {"date", httpDateNow()},
        {"content-length", std::to_string(original_body.size())}};
    auto response =
        sendRequestGetFromUpstream(request_headers, original_body, upstream_response_headers);
    EXPECT_TRUE(response->complete());
    EXPECT_EQ("200", response->headers().getStatusValue());
    EXPECT_EQ(original_body, response->body());
  }
  resetUpstreamConnection();

  // Entry should be in the store.
  EXPECT_EQ(store_->size(), 1);

  // --- Second request: entry requires revalidation, must go to upstream ---
  {
    Http::TestResponseHeaderMapImpl upstream_response_headers{
        {":status", "200"},
        {"cache-control", "public, max-age=3600"},
        {"date", httpDateNow()},
        {"content-length", std::to_string(revalidated_body.size())}};
    auto response =
        sendRequestGetFromUpstream(request_headers, revalidated_body, upstream_response_headers);
    EXPECT_TRUE(response->complete());
    EXPECT_EQ("200", response->headers().getStatusValue());
    // Should get the fresh upstream response, not the stale cached one.
    EXPECT_EQ(revalidated_body, response->body());
  }
}

// Verifies that conditional revalidation with a 304 response refreshes headers
// but serves the original cached body.
TEST_P(ExtProcCacheIntegrationTest, ConditionalRevalidation304) {
  initializeWithExtProc();

  const std::string request_path = "/test/conditional";
  const std::string cached_body = "cached body content";
  const std::string original_date = httpDateNow();
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"},
      {":path", request_path},
      {":scheme", "http"},
      {":authority", "cache-test-host"}};

  // --- First request: cache miss, store with no-cache + etag ---
  {
    Http::TestResponseHeaderMapImpl upstream_response_headers{
        {":status", "200"},
        {"cache-control", "no-cache"},
        {"etag", "\"v1\""},
        {"date", original_date},
        {"content-length", std::to_string(cached_body.size())}};
    auto response =
        sendRequestGetFromUpstream(request_headers, cached_body, upstream_response_headers);
    EXPECT_TRUE(response->complete());
    EXPECT_EQ("200", response->headers().getStatusValue());
    EXPECT_EQ(cached_body, response->body());
  }
  resetUpstreamConnection();
  EXPECT_EQ(store_->size(), 1);

  // --- Second request: upstream returns 304, body served from cache ---
  {
    auto response = codec_client_->makeHeaderOnlyRequest(request_headers);

    // The request should reach upstream with conditional headers.
    ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_,
                                                           fake_upstream_connection_));
    ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
    ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

    // Verify the ext_proc server injected If-None-Match.
    EXPECT_EQ("\"v1\"", upstream_request_->headers()
                            .get(Http::LowerCaseString("if-none-match"))[0]
                            ->value()
                            .getStringView());

    // Upstream responds with 304 Not Modified and a refreshed date.
    const std::string new_date = httpDateNow();
    Http::TestResponseHeaderMapImpl not_modified_headers{
        {":status", "304"},
        {"date", new_date},
        {"etag", "\"v1\""},
        {"cache-control", "no-cache"}};
    upstream_request_->encodeHeaders(not_modified_headers, true);

    ASSERT_TRUE(response->waitForEndStream());
    EXPECT_TRUE(response->complete());
    // Should get 200 with the original cached body, not a 304.
    EXPECT_EQ("200", response->headers().getStatusValue());
    EXPECT_EQ(cached_body, response->body());
  }
}

// Verifies request coalescing: multiple concurrent requests for the same key
// result in only one cache miss and one upstream request. The other requests
// wait and are served from the cached entry once the filler completes.
TEST_P(ExtProcCacheIntegrationTest, CoalescedCacheFill) {
  initializeWithExtProc();

  const std::string path = "/test/coalesced";
  const std::string body = "coalesced response body";
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"},
      {":path", path},
      {":scheme", "http"},
      {":authority", "cache-test-host"}};
  const std::string date = httpDateNow();
  Http::TestResponseHeaderMapImpl upstream_response_headers{
      {":status", "200"},
      {"cache-control", "public, max-age=3600"},
      {"date", date},
      {"content-length", std::to_string(body.size())}};

  // Send 3 requests concurrently (non-blocking).
  auto r1 = codec_client_->makeHeaderOnlyRequest(request_headers);
  auto r2 = codec_client_->makeHeaderOnlyRequest(request_headers);
  auto r3 = codec_client_->makeHeaderOnlyRequest(request_headers);

  // Only one request should reach upstream (the filler).
  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_,
                                                         fake_upstream_connection_));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

  // Respond to the single upstream request.
  upstream_request_->encodeHeaders(upstream_response_headers, false);
  upstream_request_->encodeData(body, true);

  // All 3 responses should complete.
  ASSERT_TRUE(r1->waitForEndStream());
  ASSERT_TRUE(r2->waitForEndStream());
  ASSERT_TRUE(r3->waitForEndStream());

  // All should get 200 with the same body.
  EXPECT_EQ("200", r1->headers().getStatusValue());
  EXPECT_EQ("200", r2->headers().getStatusValue());
  EXPECT_EQ("200", r3->headers().getStatusValue());
  EXPECT_EQ(body, r1->body());
  EXPECT_EQ(body, r2->body());
  EXPECT_EQ(body, r3->body());

  // Exactly one entry should be in the cache.
  EXPECT_EQ(store_->size(), 1);
}

// Verifies filler retry: when the first filler gets a non-cacheable response
// (500), the coordinator promotes the next waiter to filler, which goes to
// upstream and successfully fills the cache.
TEST_P(ExtProcCacheIntegrationTest, FillerFailurePromotesNextFiller) {
  initializeWithExtProc();

  const std::string path = "/test/filler-retry";
  const std::string good_body = "successfully cached";
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"},
      {":path", path},
      {":scheme", "http"},
      {":authority", "cache-test-host"}};

  // Send 3 requests concurrently.
  auto r1 = codec_client_->makeHeaderOnlyRequest(request_headers);
  auto r2 = codec_client_->makeHeaderOnlyRequest(request_headers);
  auto r3 = codec_client_->makeHeaderOnlyRequest(request_headers);

  // First filler reaches upstream.
  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_,
                                                         fake_upstream_connection_));
  FakeStreamPtr filler1_stream;
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, filler1_stream));
  ASSERT_TRUE(filler1_stream->waitForEndStream(*dispatcher_));

  // Respond with 500 (not cacheable) — triggers reportFillFailure.
  Http::TestResponseHeaderMapImpl error_headers{{":status", "500"}};
  filler1_stream->encodeHeaders(error_headers, true);

  // Second filler is promoted and reaches upstream.
  FakeStreamPtr filler2_stream;
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, filler2_stream));
  ASSERT_TRUE(filler2_stream->waitForEndStream(*dispatcher_));

  // Respond with cacheable 200.
  Http::TestResponseHeaderMapImpl good_headers{
      {":status", "200"},
      {"cache-control", "public, max-age=3600"},
      {"date", httpDateNow()},
      {"content-length", std::to_string(good_body.size())}};
  filler2_stream->encodeHeaders(good_headers, false);
  filler2_stream->encodeData(good_body, true);

  // All 3 should complete. The first filler waits for the retry instead of
  // forwarding the 500, so all clients get the successful response.
  ASSERT_TRUE(r1->waitForEndStream());
  ASSERT_TRUE(r2->waitForEndStream());
  ASSERT_TRUE(r3->waitForEndStream());

  EXPECT_EQ("200", r1->headers().getStatusValue());
  EXPECT_EQ("200", r2->headers().getStatusValue());
  EXPECT_EQ("200", r3->headers().getStatusValue());
  EXPECT_EQ(good_body, r1->body());
  EXPECT_EQ(good_body, r2->body());
  EXPECT_EQ(good_body, r3->body());

  // Cache should have the successful entry.
  EXPECT_EQ(store_->size(), 1);
}

// Verifies that when the first filler gets an uncacheable response (no-store),
// the coordinator promotes the next waiter to filler. The second filler gets
// a cacheable response and successfully fills the cache for remaining waiters.
TEST_P(ExtProcCacheIntegrationTest, UncacheableResponseRetries) {
  initializeWithExtProc();

  const std::string path = "/test/uncacheable-retry";
  const std::string uncacheable_body = "private data";
  const std::string cacheable_body = "public data";
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"},
      {":path", path},
      {":scheme", "http"},
      {":authority", "cache-test-host"}};

  // Send 3 requests concurrently.
  auto r1 = codec_client_->makeHeaderOnlyRequest(request_headers);
  auto r2 = codec_client_->makeHeaderOnlyRequest(request_headers);
  auto r3 = codec_client_->makeHeaderOnlyRequest(request_headers);

  // First filler reaches upstream.
  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_,
                                                         fake_upstream_connection_));
  FakeStreamPtr filler1_stream;
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, filler1_stream));
  ASSERT_TRUE(filler1_stream->waitForEndStream(*dispatcher_));

  // Respond with 200 but no-store (uncacheable) — triggers reportFillFailure.
  Http::TestResponseHeaderMapImpl uncacheable_headers{
      {":status", "200"},
      {"cache-control", "no-store"},
      {"date", httpDateNow()},
      {"content-length", std::to_string(uncacheable_body.size())}};
  filler1_stream->encodeHeaders(uncacheable_headers, false);
  filler1_stream->encodeData(uncacheable_body, true);

  // Second filler is promoted and reaches upstream.
  FakeStreamPtr filler2_stream;
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, filler2_stream));
  ASSERT_TRUE(filler2_stream->waitForEndStream(*dispatcher_));

  // Respond with cacheable 200.
  Http::TestResponseHeaderMapImpl cacheable_headers{
      {":status", "200"},
      {"cache-control", "public, max-age=3600"},
      {"date", httpDateNow()},
      {"content-length", std::to_string(cacheable_body.size())}};
  filler2_stream->encodeHeaders(cacheable_headers, false);
  filler2_stream->encodeData(cacheable_body, true);

  // All 3 should complete.
  ASSERT_TRUE(r1->waitForEndStream());
  ASSERT_TRUE(r2->waitForEndStream());
  ASSERT_TRUE(r3->waitForEndStream());

  // Count responses: 1 got the uncacheable body, 2 got the cacheable body.
  int uncacheable_count = 0;
  int cacheable_count = 0;
  for (auto* r : {r1.get(), r2.get(), r3.get()}) {
    EXPECT_EQ("200", r->headers().getStatusValue());
    if (r->body() == uncacheable_body) {
      uncacheable_count++;
    } else {
      EXPECT_EQ(cacheable_body, r->body());
      cacheable_count++;
    }
  }
  EXPECT_EQ(1, uncacheable_count);
  EXPECT_EQ(2, cacheable_count);

  // Cache should have the cacheable entry.
  EXPECT_EQ(store_->size(), 1);
}

// Verifies that cached responses are served via StreamedImmediateResponse with
// chunked body delivery when a small chunk_size is configured.
TEST_P(ExtProcCacheIntegrationTest, ChunkedStreamedCacheHit) {
  // Use a small chunk size so the body is split across multiple messages.
  chunk_size_ = 5;
  // Re-create the server with the new chunk size (SetUp already ran).
  cache_server_.shutdown();
  auto key_gen = std::make_shared<DefaultCacheKeyGenerator>();
  auto cacheability = std::make_shared<DefaultCacheabilityChecker>();
  auto age_calc = std::make_shared<DefaultCacheAgeCalculator>();
  cache_server_.start("127.0.0.1:0", coordinator_, key_gen, cacheability, age_calc, chunk_size_);

  initializeWithExtProc();

  const std::string request_path = "/test/chunked";
  const std::string response_body = "hello from upstream, chunked!"; // 28 bytes → 6 chunks of 5
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"},
      {":path", request_path},
      {":scheme", "http"},
      {":authority", "cache-test-host"}};
  Http::TestResponseHeaderMapImpl upstream_response_headers{
      {":status", "200"},
      {"cache-control", "public, max-age=3600"},
      {"date", httpDateNow()},
      {"content-length", std::to_string(response_body.size())}};

  // First request: cache miss, fill cache.
  {
    auto response =
        sendRequestGetFromUpstream(request_headers, response_body, upstream_response_headers);
    EXPECT_TRUE(response->complete());
    EXPECT_EQ("200", response->headers().getStatusValue());
    EXPECT_EQ(response_body, response->body());
  }
  resetUpstreamConnection();
  EXPECT_EQ(store_->size(), 1);

  // Second request: cache hit, served via StreamedImmediateResponse with
  // chunk_size=5 (body split across multiple gRPC messages).
  {
    auto response = codec_client_->makeHeaderOnlyRequest(request_headers);
    ASSERT_TRUE(response->waitForEndStream());
    EXPECT_TRUE(response->complete());
    EXPECT_EQ("200", response->headers().getStatusValue());
    EXPECT_EQ(response_body, response->body());
    EXPECT_FALSE(response->headers().get(Http::LowerCaseString("age")).empty());
  }
}

// Verifies that a request arriving after the cache fill has already started
// streaming body data still receives the complete response. The late waiter
// gets a tailing reader from the store that reads buffered data immediately
// and then follows the filler for remaining chunks.
TEST_P(ExtProcCacheIntegrationTest, LateWaiterDuringBodyFill) {
  initializeWithExtProc();

  const std::string path = "/test/late-waiter";
  const std::string body_chunk1 = "first chunk of data, ";
  const std::string body_chunk2 = "second chunk of data";
  const std::string full_body = body_chunk1 + body_chunk2;
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"},
      {":path", path},
      {":scheme", "http"},
      {":authority", "cache-test-host"}};

  // Send request 1 — becomes the filler.
  auto r1 = codec_client_->makeHeaderOnlyRequest(request_headers);

  // Wait for the filler's request to reach upstream.
  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_,
                                                         fake_upstream_connection_));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

  // Upstream sends response headers (cacheable) — this triggers storeHeaders +
  // reportHeadersAvailable in the ext_proc cache server.
  Http::TestResponseHeaderMapImpl upstream_response_headers{
      {":status", "200"},
      {"cache-control", "public, max-age=3600"},
      {"date", httpDateNow()},
      {"content-length", std::to_string(full_body.size())}};
  upstream_request_->encodeHeaders(upstream_response_headers, false);

  // Send the first body chunk — this triggers appendBody, which writes to the
  // store. Any tailing readers would get this data.
  upstream_request_->encodeData(body_chunk1, false);

  // Now send request 2 — the late arrival. The filler is mid-body-stream.
  // This request will do a coordinator lookup, find headers_released=true,
  // call store->lookup(), and get a tailing reader that has body_chunk1
  // already available in the store buffer.
  auto r2 = codec_client_->makeHeaderOnlyRequest(request_headers);

  // Send the remaining body data + end_of_stream.
  upstream_request_->encodeData(body_chunk2, true);

  // Both requests should complete with the full body.
  ASSERT_TRUE(r1->waitForEndStream());
  ASSERT_TRUE(r2->waitForEndStream());

  EXPECT_TRUE(r1->complete());
  EXPECT_EQ("200", r1->headers().getStatusValue());
  EXPECT_EQ(full_body, r1->body());

  EXPECT_TRUE(r2->complete());
  EXPECT_EQ("200", r2->headers().getStatusValue());
  EXPECT_EQ(full_body, r2->body());

  // Cache should have the entry.
  EXPECT_EQ(store_->size(), 1);
}

} // namespace
} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
