#include <string>

#include "envoy/registry/registry.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/http/utility.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer_config.h"

#include "test/common/memory/memory_test_utility.h"
#include "test/integration/http_protocol_integration.h"
#include "test/test_common/environment.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace {

// Byte offset at which the fault-injecting store starts failing reads. Chosen past
// the replay engine's first synchronous burst (ReplayChunksPerIteration * 64 KiB =
// 512 KiB) so the failing read is never issued from the reentrant read loop: by the
// time replay reaches this offset it has paused on upstream back-pressure at least
// once, so the read that hits the fault is issued from the low-watermark resume
// (inside FilterManager::callUpstreamLowWatermarkCallbacks()'s fan-out).
constexpr uint64_t FailReadAtByte = 512 * 1024;

// A test-only ExternalBuffer that offloads writes into memory (like the reference
// InMemoryExternalBuffer) but fails reads at or beyond FailReadAtByte
// synchronously -- it invokes the ReadCallback with Error reentrantly, before
// read() returns. The ExternalBuffer contract explicitly permits a synchronous
// Error completion (see external_buffer.h).
//
// This models the store whose read error, delivered on the stack of the upstream
// low-watermark fan-out, tears the stream down and unregisters the filter's
// watermark subscriber mid-iteration -- the use-after-free this test reproduces.
// Reads below the threshold succeed so replay can stream the head of the payload
// (and pause/resume against upstream back-pressure); the first read that reaches
// the threshold -- only issued from a low-watermark resume -- fails.
class FaultInjectingExternalBuffer : public ExternalBuffer {
public:
  explicit FaultInjectingExternalBuffer(Event::Dispatcher& dispatcher) : dispatcher_(dispatcher) {}
  ~FaultInjectingExternalBuffer() override { *alive_ = false; }

  void write(Buffer::InstancePtr data, WriteCallback cb) override {
    // Post the acknowledgment like the reference store: the bytes are not durable
    // until the completion runs.
    dispatcher_.post([this, alive = alive_, data = std::move(data), cb = std::move(cb)]() mutable {
      if (!*alive) {
        return;
      }
      data_.move(*data);
      cb(ExternalBufferStatus::Ok);
    });
  }

  void read(uint64_t offset, uint64_t length, ReadCallback cb) override {
    // Fail reads at/beyond the threshold, synchronously. Reads below it stream the
    // head of the payload; by the time replay reaches the threshold it has paused on
    // upstream back-pressure, so the read that trips the fault is issued from the
    // low-watermark resume -- inside FilterManager::callUpstreamLowWatermarkCallbacks()'s
    // fan-out -- which is exactly the reentrancy under test.
    if (offset >= FailReadAtByte) {
      cb(ExternalBufferStatus::Error, nullptr);
      return;
    }
    ASSERT(offset + length <= data_.length());
    auto out = std::make_unique<Buffer::OwnedImpl>();
    if (length > 0) {
      auto slice = std::make_unique<uint8_t[]>(length);
      data_.copyOut(offset, length, slice.get());
      out->add(slice.get(), length);
    }
    cb(ExternalBufferStatus::Ok, std::move(out));
  }

  uint64_t length() const override { return data_.length(); }

private:
  Event::Dispatcher& dispatcher_;
  Buffer::OwnedImpl data_;
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

// Stateless factory, shared across streams like the real backends.
class FaultInjectingExternalBufferFactory : public ExternalBufferFactory {
public:
  ExternalBufferPtr createBuffer(Event::Dispatcher& dispatcher) override {
    return std::make_unique<FaultInjectingExternalBuffer>(dispatcher);
  }
};

// Config factory that selects the fault-injecting store. Uses an empty Struct as
// its config so no message has to be added to the API proto; it is resolved from
// the typed-extension registry by its type URL, exactly like the real backends.
// Subclasses ExternalBufferConfigFactory directly rather than the CRTP base so it
// need not run PGV validation (google.protobuf.Struct has no generated validator).
class FaultInjectingExternalBufferConfigFactory : public ExternalBufferConfigFactory {
public:
  ExternalBufferFactorySharedPtr
  createExternalBufferFactory(const Protobuf::Message&,
                              Server::Configuration::FactoryContext&) override {
    return std::make_shared<FaultInjectingExternalBufferFactory>();
  }

  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return std::make_unique<Protobuf::Struct>();
  }

  std::string name() const override {
    return "envoy.http.ai_protocol_manager.external_buffers.test_fault_injecting";
  }
};

REGISTER_FACTORY(FaultInjectingExternalBufferConfigFactory, ExternalBufferConfigFactory);

} // namespace
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

namespace Envoy {
namespace {

// End-to-end coverage for the AI Protocol Manager filter. The filter offloads
// the request body into an external buffer as it arrives and replays it back
// into the filter chain once the stream ends (see filter.h). These tests drive
// real requests through a configured Envoy and assert that the upstream still
// observes the headers, the complete body (across a range of sizes), and any
// trailers unchanged -- i.e. the offload/replay round-trip is transparent.
class AiProtocolManagerIntegrationTest : public HttpProtocolIntegrationTest {
protected:
  void prependFilter() {
    config_helper_.prependFilter(R"EOF(
name: envoy.filters.http.ai_protocol_manager
typed_config:
  "@type": type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager
)EOF");
  }

  // Prepends the filter configured to offload the payload to an on-disk buffer,
  // so a large body is written to a file rather than held on the heap.
  void prependFilterWithFileSystemBuffer() {
    config_helper_.prependFilter(fmt::format(R"EOF(
name: envoy.filters.http.ai_protocol_manager
typed_config:
  "@type": type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager
  external_buffer:
    name: file_system
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.FileSystemBuffer
      buffer_path: "{}"
      manager_config:
        thread_pool:
          thread_count: 1
)EOF",
                                             TestEnvironment::temporaryDirectory()));
  }

  // Prepends the filter configured to offload into the test-only fault-injecting
  // buffer, which fails the first read issued after replay pauses (see the buffer's
  // comment). Selected by type URL (google.protobuf.Struct) from the typed-extension
  // registry.
  void prependFilterWithFaultInjectingBuffer() {
    config_helper_.prependFilter(R"EOF(
name: envoy.filters.http.ai_protocol_manager
typed_config:
  "@type": type.googleapis.com/envoy.extensions.filters.http.ai_protocol_manager.v3.AiProtocolManager
  external_buffer:
    name: test_fault_injecting
    typed_config:
      "@type": type.googleapis.com/google.protobuf.Struct
)EOF");
  }

  Http::TestRequestHeaderMapImpl requestHeaders() {
    // The authority must match the upstream test cert (*.lyft.com): with HTTP/3
    // upstreams the cluster uses TLS with auto_sni, so :authority becomes the SNI
    // validated against the served certificate.
    return Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                          {":path", "/test/long/url"},
                                          {":scheme", "http"},
                                          {":authority", "sni.lyft.com"}};
  }
};

INSTANTIATE_TEST_SUITE_P(Protocols, AiProtocolManagerIntegrationTest,
                         testing::ValuesIn(HttpProtocolIntegrationTest::getProtocolTestParams()),
                         HttpProtocolIntegrationTest::protocolTestParamsToString);

// Headers-only request: the filter must let the headers flow immediately (there
// is no payload to offload), and the round-trip must complete normally.
TEST_P(AiProtocolManagerIntegrationTest, HeaderOnly) {
  prependFilter();
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  auto response = codec_client_->makeHeaderOnlyRequest(requestHeaders());

  waitForNextUpstreamRequest();
  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ(0, upstream_request_->bodyLength());
  EXPECT_FALSE(upstream_request_->receivedData());

  upstream_request_->encodeHeaders(default_response_headers_, true);
  ASSERT_TRUE(response->waitForEndStream());
  ASSERT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// Header + body: the offloaded body must be replayed in full to the upstream.
// Parameterized over a range of sizes to exercise empty, sub-chunk, and
// multi-chunk payloads through the offload/replay path.
TEST_P(AiProtocolManagerIntegrationTest, HeaderAndBody) {
  prependFilter();
  initialize();

  for (const uint64_t body_size : {0u, 1u, 16u, 1024u, 64u * 1024u, 1024u * 1024u}) {
    codec_client_ = makeHttpConnection(lookupPort("http"));
    const std::string body(body_size, 'a');
    auto response = codec_client_->makeRequestWithBody(requestHeaders(), body);

    waitForNextUpstreamRequest();
    EXPECT_TRUE(upstream_request_->complete());
    EXPECT_EQ(body_size, upstream_request_->bodyLength());
    EXPECT_EQ(body, upstream_request_->body().toString());

    upstream_request_->encodeHeaders(default_response_headers_, true);
    ASSERT_TRUE(response->waitForEndStream());
    ASSERT_TRUE(response->complete());
    EXPECT_EQ("200", response->headers().getStatusValue());

    cleanupUpstreamAndDownstream();
  }
}

// Header + body sent as several explicit frames before end_stream. Verifies the
// filter reassembles a body delivered across multiple decodeData() calls.
TEST_P(AiProtocolManagerIntegrationTest, HeaderAndBodyMultipleFrames) {
  prependFilter();
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  auto encoder_decoder = codec_client_->startRequest(requestHeaders());
  request_encoder_ = &encoder_decoder.first;
  auto response = std::move(encoder_decoder.second);

  codec_client_->sendData(*request_encoder_, "123", false);
  codec_client_->sendData(*request_encoder_, "456", false);
  codec_client_->sendData(*request_encoder_, "789", true);

  waitForNextUpstreamRequest();
  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ("123456789", upstream_request_->body().toString());

  upstream_request_->encodeHeaders(default_response_headers_, true);
  ASSERT_TRUE(response->waitForEndStream());
  ASSERT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// Header + body + trailers: the stream is terminated by trailers rather than an
// end_stream data frame. The filter must replay the buffered body and then
// release the trailers, so the upstream observes both intact.
TEST_P(AiProtocolManagerIntegrationTest, HeaderAndBodyAndTrailers) {
  prependFilter();
  // HTTP/1.1 codecs only parse/emit trailers when explicitly enabled, on both
  // the downstream (to read client trailers) and upstream (to forward them).
  config_helper_.addConfigModifier(setEnableDownstreamTrailersHttp1());
  config_helper_.addConfigModifier(setEnableUpstreamTrailersHttp1());
  initialize();

  for (const uint64_t body_size : {0u, 16u, 1024u, 64u * 1024u}) {
    codec_client_ = makeHttpConnection(lookupPort("http"));
    auto encoder_decoder = codec_client_->startRequest(requestHeaders());
    request_encoder_ = &encoder_decoder.first;
    auto response = std::move(encoder_decoder.second);

    const std::string body(body_size, 'b');
    if (body_size > 0) {
      codec_client_->sendData(*request_encoder_, body, false);
    }
    Http::TestRequestTrailerMapImpl request_trailers{{"x-request-trailer", "trailer-value"}};
    codec_client_->sendTrailers(*request_encoder_, request_trailers);

    waitForNextUpstreamRequest();
    EXPECT_TRUE(upstream_request_->complete());
    EXPECT_EQ(body_size, upstream_request_->bodyLength());
    if (body_size > 0) {
      EXPECT_EQ(body, upstream_request_->body().toString());
    }
    ASSERT_NE(upstream_request_->trailers(), nullptr);
    EXPECT_EQ("trailer-value", upstream_request_->trailers()
                                   ->get(Http::LowerCaseString("x-request-trailer"))[0]
                                   ->value()
                                   .getStringView());

    upstream_request_->encodeHeaders(default_response_headers_, true);
    ASSERT_TRUE(response->waitForEndStream());
    ASSERT_TRUE(response->complete());
    EXPECT_EQ("200", response->headers().getStatusValue());

    cleanupUpstreamAndDownstream();
  }
}

// Trailers immediately after headers, with no body in between.
TEST_P(AiProtocolManagerIntegrationTest, HeaderAndTrailersNoBody) {
  prependFilter();
  config_helper_.addConfigModifier(setEnableDownstreamTrailersHttp1());
  config_helper_.addConfigModifier(setEnableUpstreamTrailersHttp1());
  initialize();

  codec_client_ = makeHttpConnection(lookupPort("http"));
  auto encoder_decoder = codec_client_->startRequest(requestHeaders());
  request_encoder_ = &encoder_decoder.first;
  auto response = std::move(encoder_decoder.second);

  Http::TestRequestTrailerMapImpl request_trailers{{"x-request-trailer", "trailer-value"}};
  codec_client_->sendTrailers(*request_encoder_, request_trailers);

  waitForNextUpstreamRequest();
  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ(0, upstream_request_->bodyLength());
  ASSERT_NE(upstream_request_->trailers(), nullptr);

  upstream_request_->encodeHeaders(default_response_headers_, true);
  ASSERT_TRUE(response->waitForEndStream());
  ASSERT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// A large payload must be offloaded to disk rather than held on the heap. The body
// is streamed with end_stream withheld, so the filter offloads every frame to disk
// and pins the chain -- the payload reaches neither the upstream nor Envoy's heap.
// The heap must therefore stay a small fraction of the payload; a regression that
// kept the body in memory would grow the heap by ~the payload size and fail.
TEST_P(AiProtocolManagerIntegrationTest, FileSystemBufferBoundsHeapOnLargePayload) {
  // The heap bound is protocol-independent (the payload is pinned at this filter
  // and never reaches the upstream), so assert it on a single deterministic
  // HTTP/2 <-> HTTP/2 combination where the in-process memory signal is clean.
  if (downstreamProtocol() != Http::CodecType::HTTP2 ||
      upstreamProtocol() != Http::CodecType::HTTP2) {
    return;
  }

  prependFilterWithFileSystemBuffer();
  // Cap the per-stream buffer limit so the filter's ingest back-pressure keeps the
  // not-yet-offloaded (heap-resident) window small and deterministic.
  config_helper_.setBufferLimits(256 * 1024, 256 * 1024);
  initialize();

  const uint64_t body_size = 8 * 1024 * 1024;
  const std::string body(body_size, 'a');

  codec_client_ = makeHttpConnection(lookupPort("http"));
  auto encoder_decoder = codec_client_->startRequest(requestHeaders());
  request_encoder_ = &encoder_decoder.first;
  auto response = std::move(encoder_decoder.second);

  // Baseline after `body` is allocated, so the test's own copy is not counted.
  Memory::TestUtil::MemoryTest memory_test;

  // Stream the whole body but withhold end_stream. Pass the client dispatcher to
  // waitForCounter so it is pumped while we wait -- otherwise the body is never
  // flushed from the in-process client to Envoy. Once Envoy has read it all off
  // the socket, every frame has been handed to the filter and offloaded to disk.
  codec_client_->sendData(*request_encoder_, body, false);
  test_server_->waitForCounter("http.config_test.downstream_cx_rx_bytes_total",
                               testing::Ge(body_size), TestUtility::DefaultTimeout,
                               dispatcher_.get());
  // Let the client finish draining and freeing its send buffers now that Envoy has
  // read the whole body, so the client's transient copy is not attributed to the
  // measured heap.
  for (int i = 0; i < 8; i++) {
    dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
  }

  // The body is on disk, not on the heap: only the bounded in-flight window (the
  // 256 KiB buffer limit) plus small per-request overhead should have been
  // allocated. Measured growth is a few hundred KiB; the bound is a quarter of the
  // payload, so a regression that kept the body in memory (growth ~= the payload)
  // fails loudly while leaving ample margin for allocator noise.
  EXPECT_MEMORY_LE(memory_test.consumedBytes(), body_size / 4);

  // End the stream and confirm the offloaded payload still replays to the upstream
  // intact -- the memory bound must not come at the cost of correctness.
  codec_client_->sendData(*request_encoder_, "", true);
  waitForNextUpstreamRequest();
  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_EQ(body_size, upstream_request_->bodyLength());

  upstream_request_->encodeHeaders(default_response_headers_, true);
  ASSERT_TRUE(response->waitForEndStream());
  ASSERT_TRUE(response->complete());
  EXPECT_EQ("200", response->headers().getStatusValue());
}

// Regression test: an external-buffer read that fails synchronously while replay is
// resuming from upstream back-pressure must not corrupt the shared upstream
// watermark subscriber list.
//
// During replay the filter subscribes to the upstream watermark callbacks (a
// std::list in FilterManager). When the upstream drains,
// FilterManager::callUpstreamLowWatermarkCallbacks() iterates that list with a
// range-for and invokes the filter's bridge, which resumes replay by issuing a read.
// If that read completes synchronously with Error (permitted by the ExternalBuffer
// contract), the filter fails the stream via sendLocalReply from inside the fan-out.
// That reentrant teardown corrupts the shared watermark state -- it tears down the
// upstream request (tripping a read_disable_count_ underflow assert) and erases the
// filter's own node from the list being iterated, invalidating the loop iterator.
// Under ASAN (--config=clang --config=asan) this aborts on the fan-out stack before
// the fix; the fix defers the error off the callback stack so the fan-out unwinds
// first.
//
// Choreography (see the fault-injecting buffer above):
//  1. The whole body is offloaded, then replay starts and streams it toward the
//     upstream (the first injected chunk releases the held headers and opens the
//     upstream connection).
//  2. The upstream advertises a small flow-control window, so Envoy can only put a
//     little on the wire and the rest of each replayed chunk piles up in Envoy's
//     upstream buffer past its limit. That raises the upstream high watermark and
//     pauses replay -- repeatedly, as the payload is far larger than the window.
//  3. Each time the upstream reads and grants more window, Envoy drains below the
//     low watermark and FilterManager::callUpstreamLowWatermarkCallbacks() resumes
//     replay by issuing the next read from inside its fan-out. The read that reaches
//     FailReadAtByte fails synchronously on that stack.
TEST_P(AiProtocolManagerIntegrationTest, SyncReadErrorDuringReplayResumeDoesNotCorruptWatermarks) {
  // The reproduction is protocol-independent (it hinges on the shared upstream
  // watermark list, not the wire codec); pin it to a single deterministic HTTP/2
  // <-> HTTP/2 combination to keep the flow-control timing clean.
  if (downstreamProtocol() != Http::CodecType::HTTP2 ||
      upstreamProtocol() != Http::CodecType::HTTP2) {
    return;
  }

  // Make the fake upstream advertise a small window so Envoy cannot serialize a
  // replayed chunk in one go; the remainder buffers in Envoy (post-connection, where
  // it is watermarked) and backs replay up. Without this the upstream drains as fast
  // as replay injects and replay never pauses.
  constexpr uint32_t window_size = 64 * 1024;
  envoy::config::core::v3::Http2ProtocolOptions http2_options =
      ::Envoy::Http2::Utility::initializeAndValidateOptions(
          envoy::config::core::v3::Http2ProtocolOptions())
          .value();
  http2_options.mutable_initial_stream_window_size()->set_value(window_size);
  http2_options.mutable_initial_connection_window_size()->set_value(window_size);
  mergeOptions(http2_options);

  prependFilterWithFaultInjectingBuffer();
  // Upstream limit small so the window-blocked remainder crosses it and raises the
  // high watermark; downstream limit left large so the body uploads and offloads
  // without ingest throttling.
  config_helper_.setBufferLimits(/*upstream=*/window_size, /*downstream=*/1024 * 1024);
  initialize();

  // Much larger than the window and the replay engine's first synchronous burst
  // (512 KiB), so replay pauses on back-pressure well before it reaches
  // FailReadAtByte -- the failing read is issued from a low-watermark resume.
  const std::string body(2 * 1024 * 1024, 'a');

  codec_client_ = makeHttpConnection(lookupPort("http"));
  // Send the whole request up front: the body is fully offloaded before replay, so
  // the read error is driven purely by the replay-resume path. The upstream reads in
  // the background, granting window and driving the pause/resume cycle until the
  // failing read trips the fault.
  auto response = codec_client_->makeRequestWithBody(requestHeaders(), body);

  // The synchronous read error fails the stream with a 500 local reply. The key
  // assertion is implicit: with the fix the fan-out unwinds cleanly and we get the
  // reply; without it ASAN aborts inside the watermark-callback loop.
  ASSERT_TRUE(response->waitForEndStream());
  ASSERT_TRUE(response->complete());
  EXPECT_EQ("500", response->headers().getStatusValue());
}

} // namespace
} // namespace Envoy
