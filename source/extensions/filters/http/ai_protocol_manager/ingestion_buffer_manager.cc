#include "source/extensions/filters/http/ai_protocol_manager/ingestion_buffer_manager.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "envoy/buffer/buffer.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"
#include "source/common/common/logger.h"
#include "source/extensions/filters/http/ai_protocol_manager/external_buffer.h"
#include "source/extensions/filters/http/ai_protocol_manager/filter_chain_bridge.h"
#include "source/extensions/filters/http/ai_protocol_manager/payload_reader.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

IngestionBufferManager::IngestionBufferManager(Config config, ExternalBufferFactory& buffer_factory,
                                               FilterChainBridge& bridge)
    : config_(config), buffer_factory_(buffer_factory), bridge_(bridge) {
  bridge_.registerClient(bridge_adapter_);
}

void IngestionBufferManager::onDestroy() {
  if (destroyed_) {
    return;
  }
  // Keep `this` alive on the stack until teardown finishes when self-owned.
  IngestionBufferManagerPtr self = std::move(self_owner_);
  destroyed_ = true;
  *alive_ = false;
  bridge_.unregisterClient(bridge_adapter_);
  PayloadReaderSharedPtr reader = exported_reader_.lock();
  exported_reader_.reset();
  const bool had_buffer = (buffer_ != nullptr);
  const uint64_t unacked = had_buffer ? (in_flight_write_size_ + pending_.length()) : 0;
  write_in_flight_ = false;
  in_flight_write_size_ = 0;
  pending_.drain(pending_.length());
  // Dropping buffer_ releases this manager's share; if an already-durable PayloadReader was
  // exported, it retains its own shared_ptr<ExternalBuffer>, whereas any still-flushing buffer is
  // destroyed immediately here to cancel pending async writes.
  buffer_.reset();
  if (unacked > 0) {
    bridge_.releaseUnacked(unacked);
  }
  if (reader != nullptr && !reader->allBytesReadable()) {
    reader->failExternalHandoff();
  }
}

void IngestionBufferManager::onData(Buffer::Instance& data, bool end_stream) {
  if (destroyed_ || write_failed_) {
    if (end_stream) {
      end_stream_seen_ = true;
    }
    data.drain(data.length());
    return;
  }
  if (end_stream_seen_) {
    IS_ENVOY_BUG("ai_protocol_manager: data after end of stream");
    data.drain(data.length());
    return;
  }
  std::shared_ptr<bool> alive = alive_;
  const uint64_t accepted = data.length();
  total_length_ += accepted;
  // Queue the bytes, taking ownership now so the filter chain's buffer reference does not dangle
  // across the asynchronous offload. This ownership/serialization is storage-agnostic, so it lives
  // here once rather than in every ExternalBuffer.
  pending_.move(data);
  if (end_stream) {
    // Set before flushing, so this frame's write carries end_stream.
    end_stream_seen_ = true;
  }

  if (buffer_ == nullptr) {
    if (pending_.length() <= config_.max_in_memory_bytes) {
      // Still within the in-memory tier. These bytes are readable where they are, so they are not
      // pending durability and there is nothing to write.
      ENVOY_LOG(trace, "ai_protocol_manager: holding {} bytes in memory ({} total)", accepted,
                pending_.length());
      return;
    }
    ENVOY_LOG(debug, "ai_protocol_manager: offloading, {} bytes exceeds the in-memory limit of {}",
              pending_.length(), config_.max_in_memory_bytes);
    buffer_ = buffer_factory_.createBuffer(bridge_.dispatcher());
    // Everything held so far now needs a write to become readable, so all of it counts.
    bridge_.addUnacked(pending_.length());
    if (!*alive || destroyed_) {
      return;
    }
    maybeIssueWrite();
    return;
  }

  ENVOY_LOG(trace, "ai_protocol_manager: offloading {} bytes", accepted);
  // maybeIssueWrite() flushes the queued backlog to the buffer as a single write once it is worth
  // doing (batching small frames up to WriteFlushThreshold).
  bridge_.addUnacked(accepted);
  if (!*alive || destroyed_) {
    return;
  }
  maybeIssueWrite();
}

void IngestionBufferManager::endStream() {
  end_stream_seen_ = true;
  if (destroyed_ || write_failed_) {
    return;
  }
  ENVOY_LOG(trace, "ai_protocol_manager: stream complete");
  // No more data is coming: flush whatever is batched, even below the threshold.
  maybeIssueWrite();
}

const Buffer::Instance* IngestionBufferManager::inMemoryBytes() const {
  if (destroyed_ || write_failed_ || buffer_ != nullptr) {
    return nullptr;
  }
  return &pending_;
}

PayloadReaderSharedPtr
IngestionBufferManager::exportPayloadReader(IngestionBufferManagerPtr manager) {
  ASSERT(manager != nullptr);
  ASSERT(manager->end_stream_seen_);
  if (manager->destroyed_ || manager->write_failed_) {
    return PayloadReader::createFailed(manager->total_length_);
  }
  if (manager->buffer_ == nullptr) {
    return PayloadReader::fromInMemory(manager->pending_);
  }
  if (!manager->write_in_flight_ && manager->pending_.length() == 0) {
    return PayloadReader::fromExternalBuffer(std::move(manager->buffer_), manager->total_length_);
  }
  ASSERT(manager->write_in_flight_);
  const uint64_t total_length = manager->total_length_;
  IngestionBufferManager* raw = manager.get();
  PayloadReaderSharedPtr reader = PayloadReader::createPendingExternal(total_length);
  raw->exported_reader_ = reader;
  raw->self_owner_ = std::move(manager);
  return reader;
}

void IngestionBufferManager::maybeIssueWrite() {
  // Nothing to do while the payload is still within the in-memory tier: there is no store yet, and
  // the bytes are already readable in pending_.
  //
  // Honor the buffer's single-writer contract: only one write outstanding. The
  // rest of the backlog stays in pending_ until this one completes.
  if (buffer_ == nullptr || write_in_flight_ || write_failed_) {
    return;
  }
  const bool has_pending_data = pending_.length() > 0;
  const bool needs_terminal_write = end_stream_seen_ && !end_stream_flushed_;
  // Nothing to write if there is no pending data and no terminal write is needed.
  if (!has_pending_data && !needs_terminal_write) {
    return;
  }
  // Batch small frames into a chunk-sized write instead of writing each one: with
  // a single write in flight at a time, per-frame writes would stream many tiny
  // writes to the backing store. Holding a sub-threshold backlog costs nothing on
  // the critical path as long as nothing needs those bytes durable yet. Flush early
  // regardless of size once the stream has ended (nothing more is coming) or the
  // source is paused for back-pressure (the backlog cannot grow, so waiting would
  // stall -- this also guarantees progress when the buffer limit is below the
  // threshold).
  if (!end_stream_seen_ && !bridge_.ingestPaused() && pending_.length() < WriteFlushThreshold) {
    return;
  }
  auto owned = std::make_unique<Buffer::OwnedImpl>();
  in_flight_write_size_ = pending_.length();
  owned->move(pending_);
  write_in_flight_ = true;
  end_stream_flushed_ = end_stream_seen_;
  std::shared_ptr<bool> alive = alive_;
  buffer_->write(std::move(owned), end_stream_seen_, [this, alive](ExternalBufferStatus status) {
    if (!*alive || destroyed_) {
      return;
    }
    onWriteComplete(status);
  });
}

void IngestionBufferManager::onWriteComplete(ExternalBufferStatus status) {
  std::shared_ptr<bool> alive = alive_;
  // A conforming store cancels pending completions when it is destroyed in
  // onDestroy() (see ExternalBuffer), so this never fires once detached.
  ASSERT(!destroyed_);
  const uint64_t acked = in_flight_write_size_;
  write_in_flight_ = false;
  in_flight_write_size_ = 0;
  // Let the source resume if the not-yet-durable total has fallen below the low
  // watermark.
  bridge_.releaseUnacked(acked);
  if (!*alive || destroyed_) {
    return;
  }
  if (self_owner_ != nullptr && exported_reader_.expired()) {
    onDestroy();
    return;
  }
  if (status != ExternalBufferStatus::Ok) {
    write_failed_ = true;
    PayloadReaderSharedPtr reader = exported_reader_.lock();
    IngestionBufferManagerPtr self = std::move(self_owner_);
    onExternalBufferError();
    if (reader != nullptr) {
      reader->failExternalHandoff();
    }
    return;
  }

  maybeIssueWrite();
  if (!*alive || destroyed_) {
    return;
  }
  if (!write_in_flight_ && pending_.length() == 0) {
    if (PayloadReaderSharedPtr reader = exported_reader_.lock(); reader != nullptr) {
      std::shared_ptr<ExternalBuffer> buf = std::move(buffer_);
      reader->completeExternalHandoff(std::move(buf));
    }
    if (self_owner_ != nullptr) {
      onDestroy();
    }
  }
}

void IngestionBufferManager::onExternalBufferError() {
  ENVOY_LOG(warn, "ai_protocol_manager: external buffer I/O error, failing stream");
  bridge_.notifyUnrecoverableError();
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
