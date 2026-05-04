#pragma once

#include <cstddef>
#include <cstdint>

#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_payload.h"

#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

// MmapPayloadStore offloads large payloads to a temporary file via mmap,
// keeping only small fields (≤ max_inline_bytes) in process heap memory.
//
// Backing file: created with mkstemp, immediately unlinked. The file exists
// only as an open file descriptor and is reclaimed automatically when the
// store is destroyed. This makes it anonymous from the filesystem's perspective
// while still being page-cache backed — the OS can evict pages under memory
// pressure without the payload being lost, as long as the fd is open.
//
// Layout: a bump-allocated arena. Each large payload is appended sequentially
// to the file; PayloadRef::External stores {offset, length} so that fetch()
// can read the slice back from the mapped region.
//
// Capacity growth: starts at kInitialCapacity bytes, doubles on overflow.
// ftruncate extends the backing file; mremap (Linux) or munmap+mmap (macOS)
// extends the mapping in place or at a new address.
//
// Fallback: if mkstemp or any mmap/ftruncate call fails, large payloads fall
// back to PayloadRef::Buffered (heap Buffer::OwnedImpl) so the store is always
// functional.
//
// Thread safety: not thread-safe. One store per request stream is the
// intended pattern (matching Envoy's single-threaded filter chain).
class MmapPayloadStore : public PayloadStore {
public:
  static constexpr size_t kDefaultMaxInlineBytes = 4096;
  static constexpr size_t kInitialCapacity       = 1U << 16; // 64 KB

  explicit MmapPayloadStore(absl::string_view tmp_dir   = "/tmp",
                            size_t           max_inline = kDefaultMaxInlineBytes);
  ~MmapPayloadStore() override;

  MmapPayloadStore(const MmapPayloadStore&)            = delete;
  MmapPayloadStore& operator=(const MmapPayloadStore&) = delete;

  PayloadRef store(std::string data,       PayloadKind kind) override;
  PayloadRef store(Buffer::Instance& data, PayloadKind kind) override;

  // Synchronous. External refs are read directly from the mmap region.
  void fetch(const PayloadRef& ref, FetchCallback cb) override;

  // Total bytes written to the backing file. Exposed for testing.
  size_t bytesWritten() const { return write_offset_; }

private:
  // Ensures at least `needed` additional bytes are available in the mapping.
  // Grows the backing file and remaps if necessary.
  // Returns false on any system error; caller falls back to Buffered.
  bool ensureSpace(size_t needed);

  // Copies `len` bytes from `src` into the mmap region at write_offset_,
  // advances write_offset_, and returns the starting offset.
  uint64_t appendBytes(const void* src, size_t len);

  int      fd_{-1};
  uint8_t* map_{nullptr};
  size_t   capacity_{0};
  size_t   write_offset_{0};
  size_t   max_inline_bytes_;
};

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
