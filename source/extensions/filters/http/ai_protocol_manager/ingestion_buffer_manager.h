#pragma once

#include <cstdint>
#include <memory>

#include "envoy/buffer/buffer.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/logger.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer.h"
#include "source/extensions/filters/http/ai_protocol_manager/filter_chain_bridge.h"
#include "source/extensions/filters/http/ai_protocol_manager/payload_reader.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

class IngestionBufferManager;
using IngestionBufferManagerPtr = std::unique_ptr<IngestionBufferManager>;

// Owns callback-driven HTTP payload ingestion into an in-memory queue or ExternalBuffer with
// ingest back-pressure, exporting a PayloadReader once endStream() is reached.
//
// Ingest flow control: the manager serializes writes (at most one outstanding) and queues the
// backlog itself, batching small frames into chunk-sized writes (WriteFlushThreshold) so a store
// that keeps up does not get a stream of tiny writes. It reports the not-yet-durable byte count
// (queued plus in-flight) to the bridge, which pushes back on the data source when the backing
// store cannot keep up.
class IngestionBufferManager : public Logger::Loggable<Logger::Id::filter> {
public:
  struct Config {
    // Maximum bytes held in memory before spilling to an ExternalBuffer. Payloads within this
    // limit stay in memory without storage IO.
    uint64_t max_in_memory_bytes{0};
  };

  // `bridge` is shared by every IngestionBufferManager on the path and must outlive them all.
  IngestionBufferManager(Config config, ExternalBufferFactory& buffer_factory,
                         FilterChainBridge& bridge);

  ~IngestionBufferManager() {
    onDestroy();
    *alive_ = false;
  }

  // Offloads `data` into the external buffer (batching small frames). `end_stream` marks `data`
  // as the last frame of the body.
  void onData(Buffer::Instance& data, bool end_stream = false);

  // Signals that the full body has been ingested, flushing any batched backlog to the buffer so
  // all of it becomes durable and readable.
  void endStream();

  // Total byte length accepted so far.
  uint64_t length() const { return total_length_; }

  // True until the first non-empty onData().
  bool empty() const { return total_length_ == 0; }

  // Every accepted byte, while the payload is still within the in-memory tier; nullptr once it has
  // been offloaded, failed, or destroyed.
  const Buffer::Instance* inMemoryBytes() const;

  // Consumes `manager` (must be called only after endStream() or onData(..., /*end_stream=*/true))
  // and returns a reference-counted PayloadReader over the completed payload. If all bytes are
  // already durable (or failed), `manager` is destroyed immediately; if an ExternalBuffer flush is
  // still in flight, `manager` becomes self-owned until the terminal write completes, fails, all
  // readers are dropped, or the bridge detaches.
  static PayloadReaderSharedPtr exportPayloadReader(IngestionBufferManagerPtr manager);

  // Detaches the manager from the filter chain: unregisters from the bridge, cancels any
  // still-flushing PayloadReader handoff, releases unshared ExternalBuffer ownership, and returns
  // unacked bytes. Idempotent.
  void onDestroy();

private:
  class BridgeAdapter : public FilterChainBridge::Client {
  public:
    explicit BridgeAdapter(IngestionBufferManager& owner) : owner_(owner) {}
    void onBridgeDetached() override { owner_.onDestroy(); }
    void onIngestPaused() override { owner_.maybeIssueWrite(); }

  private:
    IngestionBufferManager& owner_;
  };

  void maybeIssueWrite();
  void onWriteComplete(ExternalBufferStatus status);
  void onExternalBufferError();

  // Minimum queued backlog that triggers a write while the stream is still open and the source is
  // flowing.
  static constexpr uint64_t WriteFlushThreshold = 64 * 1024;

  Config config_;
  ExternalBufferFactory& buffer_factory_;
  FilterChainBridge& bridge_;
  BridgeAdapter bridge_adapter_{*this};
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
  std::shared_ptr<ExternalBuffer> buffer_;
  std::weak_ptr<PayloadReader> exported_reader_;
  // Non-null only after exportPayloadReader() when an ExternalBuffer flush is still in flight;
  // released on write completion/error, reader expiration, or bridge detachment (onDestroy()).
  IngestionBufferManagerPtr self_owner_;

  uint64_t total_length_{0};
  bool end_stream_seen_{false};
  bool end_stream_flushed_{false};
  Buffer::OwnedImpl pending_;
  bool write_in_flight_{false};
  bool write_failed_{false};
  uint64_t in_flight_write_size_{0};
  bool destroyed_{false};
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
