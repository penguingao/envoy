#include "envoy/extensions/filters/http/ext_proc/v3/ext_proc.pb.h"
#include "envoy/extensions/filters/http/parallel_ext_proc/v3/parallel_ext_proc.pb.h"
#include "envoy/extensions/filters/network/http_connection_manager/v3/http_connection_manager.pb.h"
#include "envoy/service/ext_proc/v3/external_processor.pb.h"

#include "source/common/protobuf/utility.h"

#include "test/common/grpc/grpc_client_integration.h"
#include "test/common/http/common.h"
#include "test/integration/http_integration.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ParallelExtProc {

using envoy::service::ext_proc::v3::HeadersResponse;
using envoy::service::ext_proc::v3::HttpHeaders;
using envoy::service::ext_proc::v3::ProcessingRequest;
using envoy::service::ext_proc::v3::ProcessingResponse;
using Http::LowerCaseString;
using testing::_;
using testing::Not;

class ParallelExtProcIntegrationTest : public HttpIntegrationTest,
                                       public Grpc::GrpcClientIntegrationParamTest {
protected:
  ParallelExtProcIntegrationTest() : HttpIntegrationTest(Http::CodecType::HTTP2, ipVersion()) {}

  void createUpstreams() override {
    HttpIntegrationTest::createUpstreams();
    // Create separate fake upstreams for each ext_proc gRPC server.
    for (int i = 0; i < grpc_upstream_count_; ++i) {
      grpc_upstreams_.push_back(&addFakeUpstream(Http::CodecType::HTTP2));
    }
  }

  void TearDown() override {
    for (auto& conn : processor_connections_) {
      if (conn) {
        ASSERT_TRUE(conn->close());
        ASSERT_TRUE(conn->waitForDisconnect());
      }
    }
    cleanupUpstreamAndDownstream();
  }

  void initializeConfig() {
    config_helper_.addConfigModifier(
        [this](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
          // Create clusters for each ext_proc gRPC server. These need HTTP/2.
          for (int i = 0; i < grpc_upstream_count_; ++i) {
            auto* server_cluster = bootstrap.mutable_static_resources()->add_clusters();
            server_cluster->MergeFrom(bootstrap.static_resources().clusters()[0]);
            std::string cluster_name = absl::StrCat("ext_proc_server_", i);
            server_cluster->set_name(cluster_name);
            server_cluster->mutable_load_assignment()->set_cluster_name(cluster_name);
            ConfigHelper::setHttp2(*server_cluster);
          }

          // Build the ParallelExternalProcessor config.
          envoy::extensions::filters::http::parallel_ext_proc::v3::ParallelExternalProcessor
              parallel_config;

          for (int i = 0; i < grpc_upstream_count_; ++i) {
            auto* proc = parallel_config.add_processors();
            proc->set_name(absl::StrCat("processor_", i));
            proc->set_priority(i); // processor_0 has highest priority (lowest number)

            auto* ext_proc_config = proc->mutable_ext_proc_config();
            std::string cluster_name = absl::StrCat("ext_proc_server_", i);
            setGrpcService(*ext_proc_config->mutable_grpc_service(), cluster_name,
                           grpc_upstreams_[i]->localAddress());

            // Send request and response headers by default.
            ext_proc_config->mutable_processing_mode()->set_request_header_mode(
                envoy::extensions::filters::http::ext_proc::v3::ProcessingMode::SEND);
            ext_proc_config->mutable_processing_mode()->set_response_header_mode(
                envoy::extensions::filters::http::ext_proc::v3::ProcessingMode::SKIP);
            ext_proc_config->mutable_processing_mode()->set_request_body_mode(
                request_body_mode_);
            // Use a long message timeout so tests that simulate slow backends
            // don't trigger the default 200ms ext_proc timeout.
            ext_proc_config->mutable_message_timeout()->set_seconds(30);
          }

          // Apply failure policy if set.
          if (failure_mode_allow_) {
            parallel_config.mutable_failure_policy()->set_failure_mode_allow(true);
          }

          // Add the parallel_ext_proc filter.
          envoy::extensions::filters::network::http_connection_manager::v3::HttpFilter filter;
          filter.set_name("envoy.filters.http.parallel_ext_proc");
          filter.mutable_typed_config()->PackFrom(parallel_config);
          config_helper_.prependFilter(MessageUtil::getJsonStringFromMessageOrError(filter));
        });
  }

  IntegrationStreamDecoderPtr sendDownstreamRequest(
      absl::optional<std::function<void(Http::RequestHeaderMap& headers)>> modify_headers =
          absl::nullopt) {
    auto conn = makeClientConnection(lookupPort("http"));
    codec_client_ = makeHttpConnection(std::move(conn));
    Http::TestRequestHeaderMapImpl headers;
    HttpTestUtility::addDefaultHeaders(headers);
    if (modify_headers) {
      (*modify_headers)(headers);
    }
    return codec_client_->makeHeaderOnlyRequest(headers);
  }

  // Process request headers from a specific ext_proc server.
  // Since both processors run in parallel, the order of gRPC messages is non-deterministic.
  // This method establishes connection and waits for the first message.
  void processRequestHeaders(
      int processor_index, bool first_message,
      absl::optional<std::function<bool(const HttpHeaders&, HeadersResponse&)>> cb) {
    ProcessingRequest request;
    auto& conn = processor_connections_[processor_index];
    auto& stream = processor_streams_[processor_index];

    if (first_message) {
      ASSERT_TRUE(grpc_upstreams_[processor_index]->waitForHttpConnection(*dispatcher_, conn));
      ASSERT_TRUE(conn->waitForNewStream(*dispatcher_, stream));
    }
    ASSERT_TRUE(stream->waitForGrpcMessage(*dispatcher_, request));
    ASSERT_TRUE(request.has_request_headers());
    if (first_message) {
      stream->startGrpcStream();
    }
    ProcessingResponse response;
    auto* headers_resp = response.mutable_request_headers();
    const bool send_reply = !cb || (*cb)(request.request_headers(), *headers_resp);
    if (send_reply) {
      stream->sendGrpcMessage(response);
    }
  }

  void verifyDownstreamResponse(IntegrationStreamDecoder& response, int status_code) {
    ASSERT_TRUE(response.waitForEndStream());
    EXPECT_TRUE(response.complete());
    EXPECT_EQ(std::to_string(status_code), response.headers().getStatusValue());
  }

  void handleUpstreamRequest() {
    ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
    ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
    ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));
    upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  }

  int grpc_upstream_count_ = 2;
  bool failure_mode_allow_ = false;
  // Body mode applied to every processor's ext_proc config.
  envoy::extensions::filters::http::ext_proc::v3::ProcessingMode_BodySendMode
      request_body_mode_ =
          envoy::extensions::filters::http::ext_proc::v3::ProcessingMode::NONE;
  std::vector<FakeUpstream*> grpc_upstreams_;
  std::vector<FakeHttpConnectionPtr> processor_connections_{2};
  std::vector<FakeStreamPtr> processor_streams_{2};
};

INSTANTIATE_TEST_SUITE_P(IpVersionsClientType, ParallelExtProcIntegrationTest,
                         GRPC_CLIENT_INTEGRATION_PARAMS);

// Test that two ext_proc processors are called in parallel and both
// can add headers. The merged result should contain headers from both.
TEST_P(ParallelExtProcIntegrationTest, TwoProcessorsAddHeaders) {
  initializeConfig();
  HttpIntegrationTest::initialize();

  auto response = sendDownstreamRequest();

  // Both processors run in parallel, so we process them in arbitrary order.
  // Processor 0 adds x-from-proc-0.
  processRequestHeaders(0, true,
                        [](const HttpHeaders&, HeadersResponse& headers_resp) {
                          auto* mutation =
                              headers_resp.mutable_response()->mutable_header_mutation();
                          auto* set_hdr = mutation->add_set_headers();
                          set_hdr->mutable_append()->set_value(false);
                          set_hdr->mutable_header()->set_key("x-from-proc-0");
                          set_hdr->mutable_header()->set_raw_value("value-0");
                          return true;
                        });

  // Processor 1 adds x-from-proc-1.
  processRequestHeaders(1, true,
                        [](const HttpHeaders&, HeadersResponse& headers_resp) {
                          auto* mutation =
                              headers_resp.mutable_response()->mutable_header_mutation();
                          auto* set_hdr = mutation->add_set_headers();
                          set_hdr->mutable_append()->set_value(false);
                          set_hdr->mutable_header()->set_key("x-from-proc-1");
                          set_hdr->mutable_header()->set_raw_value("value-1");
                          return true;
                        });

  // Verify the upstream request has headers from both processors.
  handleUpstreamRequest();
  EXPECT_THAT(upstream_request_->headers(), ContainsHeader("x-from-proc-0", "value-0"));
  EXPECT_THAT(upstream_request_->headers(), ContainsHeader("x-from-proc-1", "value-1"));

  verifyDownstreamResponse(*response, 200);
}

// Test that when two processors set the same header, the higher-priority
// processor (lower priority number) wins.
TEST_P(ParallelExtProcIntegrationTest, PriorityResolvesConflicts) {
  initializeConfig();
  HttpIntegrationTest::initialize();

  auto response = sendDownstreamRequest();

  // Processor 0 (priority=0, highest) sets x-conflict to "from-0".
  processRequestHeaders(0, true,
                        [](const HttpHeaders&, HeadersResponse& headers_resp) {
                          auto* mutation =
                              headers_resp.mutable_response()->mutable_header_mutation();
                          auto* set_hdr = mutation->add_set_headers();
                          set_hdr->mutable_append()->set_value(false);
                          set_hdr->mutable_header()->set_key("x-conflict");
                          set_hdr->mutable_header()->set_raw_value("from-0");
                          return true;
                        });

  // Processor 1 (priority=1, lower) sets x-conflict to "from-1".
  processRequestHeaders(1, true,
                        [](const HttpHeaders&, HeadersResponse& headers_resp) {
                          auto* mutation =
                              headers_resp.mutable_response()->mutable_header_mutation();
                          auto* set_hdr = mutation->add_set_headers();
                          set_hdr->mutable_append()->set_value(false);
                          set_hdr->mutable_header()->set_key("x-conflict");
                          set_hdr->mutable_header()->set_raw_value("from-1");
                          return true;
                        });

  // Processor 0 wins because it has higher priority (lower number).
  handleUpstreamRequest();
  EXPECT_THAT(upstream_request_->headers(), ContainsHeader("x-conflict", "from-0"));

  verifyDownstreamResponse(*response, 200);
}

// Test that one processor can remove a header that exists in the original request.
TEST_P(ParallelExtProcIntegrationTest, ProcessorRemovesHeader) {
  initializeConfig();
  HttpIntegrationTest::initialize();

  auto response = sendDownstreamRequest([](Http::RequestHeaderMap& headers) {
    headers.addCopy(LowerCaseString("x-remove-me"), "yes");
  });

  // Processor 0 removes x-remove-me.
  processRequestHeaders(0, true,
                        [](const HttpHeaders&, HeadersResponse& headers_resp) {
                          auto* mutation =
                              headers_resp.mutable_response()->mutable_header_mutation();
                          mutation->add_remove_headers("x-remove-me");
                          return true;
                        });

  // Processor 1 does nothing (sends empty response).
  processRequestHeaders(1, true,
                        [](const HttpHeaders&, HeadersResponse&) { return true; });

  handleUpstreamRequest();
  EXPECT_THAT(upstream_request_->headers(), Not(ContainsHeader("x-remove-me", _)));

  verifyDownstreamResponse(*response, 200);
}

// Test that processors doing nothing (no mutations) still works.
TEST_P(ParallelExtProcIntegrationTest, NoMutations) {
  initializeConfig();
  HttpIntegrationTest::initialize();

  auto response = sendDownstreamRequest();

  // Both processors respond with no mutations.
  processRequestHeaders(0, true,
                        [](const HttpHeaders&, HeadersResponse&) { return true; });
  processRequestHeaders(1, true,
                        [](const HttpHeaders&, HeadersResponse&) { return true; });

  handleUpstreamRequest();
  verifyDownstreamResponse(*response, 200);
}

// Verify flow control on the request body: the slowest ext_proc dictates the
// processing speed. The upstream must not see anything (neither headers nor
// body) until all ext_proc processors have responded.
TEST_P(ParallelExtProcIntegrationTest, RequestBodyGatedBySlowestProcessor) {
  initializeConfig();
  HttpIntegrationTest::initialize();

  // Start a POST request and send the body.
  const std::string body_data = "parallel-ext-proc-body-payload";
  codec_client_ = makeHttpConnection(lookupPort("http"));
  Http::TestRequestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  headers.setMethod("POST");
  auto encoder_decoder = codec_client_->startRequest(headers);
  auto& request_encoder = encoder_decoder.first;
  auto response = std::move(encoder_decoder.second);
  codec_client_->sendData(request_encoder, body_data, /*end_stream=*/true);

  // Wait for both processors to receive the header request, but do not reply yet.
  // Processor 0 will be the slow one.
  ProcessingRequest slow_request;
  ASSERT_TRUE(grpc_upstreams_[0]->waitForHttpConnection(*dispatcher_, processor_connections_[0]));
  ASSERT_TRUE(processor_connections_[0]->waitForNewStream(*dispatcher_, processor_streams_[0]));
  ASSERT_TRUE(processor_streams_[0]->waitForGrpcMessage(*dispatcher_, slow_request));
  ASSERT_TRUE(slow_request.has_request_headers());
  processor_streams_[0]->startGrpcStream();

  // Processor 1 (fast) receives the header request and immediately responds.
  processRequestHeaders(
      1, /*first_message=*/true, [](const HttpHeaders&, HeadersResponse& resp) {
        auto* mut = resp.mutable_response()->mutable_header_mutation()->add_set_headers();
        mut->mutable_header()->set_key("x-from-fast");
        mut->mutable_header()->set_raw_value("yes");
        return true;
      });

  // The main filter chain must still be paused because processor 0 has not
  // replied yet. Confirm that the upstream cluster sees no connection within
  // a short window.
  FakeHttpConnectionPtr premature_upstream_conn;
  EXPECT_FALSE(fake_upstreams_[0]->waitForHttpConnection(
      *dispatcher_, premature_upstream_conn, std::chrono::milliseconds(500)));
  EXPECT_EQ(premature_upstream_conn, nullptr);

  // Now the slow processor replies. This should release the filter chain.
  ProcessingResponse slow_response;
  auto* slow_mut = slow_response.mutable_request_headers()
                       ->mutable_response()
                       ->mutable_header_mutation()
                       ->add_set_headers();
  slow_mut->mutable_header()->set_key("x-from-slow");
  slow_mut->mutable_header()->set_raw_value("yes");
  processor_streams_[0]->sendGrpcMessage(slow_response);

  // The upstream should now receive the full request (headers + body).
  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

  // Body forwarded verbatim, and header mutations from both processors applied.
  EXPECT_EQ(body_data, upstream_request_->body().toString());
  EXPECT_THAT(upstream_request_->headers(), ContainsHeader("x-from-fast", "yes"));
  EXPECT_THAT(upstream_request_->headers(), ContainsHeader("x-from-slow", "yes"));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  verifyDownstreamResponse(*response, 200);
}

// Verify that every ext_proc processor receives the request body (as a
// ProcessingRequest::request_body message) before it responds, and that the
// upstream cluster does not see the request until every processor has
// acknowledged both the headers and the body.
TEST_P(ParallelExtProcIntegrationTest, ProcessorsReceiveBodyBeforeResponding) {
  // Configure every processor to receive the body buffered.
  request_body_mode_ = envoy::extensions::filters::http::ext_proc::v3::ProcessingMode::BUFFERED;
  initializeConfig();
  HttpIntegrationTest::initialize();

  // Send a POST with a body.
  const std::string body_data = "parallel-ext-proc-observed-body";
  codec_client_ = makeHttpConnection(lookupPort("http"));
  Http::TestRequestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  headers.setMethod("POST");
  auto encoder_decoder = codec_client_->startRequest(headers);
  auto& request_encoder = encoder_decoder.first;
  auto response = std::move(encoder_decoder.second);
  codec_client_->sendData(request_encoder, body_data, /*end_stream=*/true);

  // Drive each processor: consume the header message, then consume the body
  // message (which must arrive BEFORE we respond). For each, first check
  // that the upstream has not yet seen the request.
  for (int i = 0; i < grpc_upstream_count_; ++i) {
    ProcessingRequest header_req;
    ASSERT_TRUE(grpc_upstreams_[i]->waitForHttpConnection(*dispatcher_, processor_connections_[i]));
    ASSERT_TRUE(processor_connections_[i]->waitForNewStream(*dispatcher_, processor_streams_[i]));
    ASSERT_TRUE(processor_streams_[i]->waitForGrpcMessage(*dispatcher_, header_req));
    ASSERT_TRUE(header_req.has_request_headers());
    processor_streams_[i]->startGrpcStream();

    // Respond to the headers so ext_proc transitions into body processing.
    ProcessingResponse header_resp;
    header_resp.mutable_request_headers();
    processor_streams_[i]->sendGrpcMessage(header_resp);

    // The body message must be delivered to the processor BEFORE the
    // processor sends its body response.
    ProcessingRequest body_req;
    ASSERT_TRUE(processor_streams_[i]->waitForGrpcMessage(*dispatcher_, body_req));
    ASSERT_TRUE(body_req.has_request_body());
    EXPECT_EQ(body_data, body_req.request_body().body());
    EXPECT_TRUE(body_req.request_body().end_of_stream());
  }

  // At this point every processor has received headers and body but none has
  // sent a body response. The upstream must still see nothing.
  FakeHttpConnectionPtr premature_conn;
  EXPECT_FALSE(fake_upstreams_[0]->waitForHttpConnection(
      *dispatcher_, premature_conn, std::chrono::milliseconds(500)));
  EXPECT_EQ(premature_conn, nullptr);

  // Now every processor responds to the body. Only after the last response
  // should the upstream see the request.
  for (int i = 0; i < grpc_upstream_count_; ++i) {
    ProcessingResponse body_resp;
    body_resp.mutable_request_body();
    processor_streams_[i]->sendGrpcMessage(body_resp);
  }

  // Upstream receives the complete request.
  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, fake_upstream_connection_));
  ASSERT_TRUE(fake_upstream_connection_->waitForNewStream(*dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));
  EXPECT_EQ(body_data, upstream_request_->body().toString());

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);
  verifyDownstreamResponse(*response, 200);
}

} // namespace ParallelExtProc
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
