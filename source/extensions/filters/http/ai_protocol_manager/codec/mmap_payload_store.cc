#include "source/extensions/filters/http/ai_protocol_manager/codec/mmap_payload_store.h"

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "envoy/event/dispatcher.h"

#include "source/common/common/assert.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

MmapPayloadStore::MmapPayloadStore(absl::string_view tmp_dir, size_t max_inline)
    : max_inline_bytes_(max_inline) {
  std::string tmpl = absl::StrCat(tmp_dir, "/envoy_payload_XXXXXX");
  fd_ = ::mkstemp(tmpl.data());
  if (fd_ == -1) {
    return; // mmap path unavailable; large payloads will fall back to Buffered
  }
  // Unlink immediately: the file is anonymous from the filesystem's perspective
  // but remains accessible via fd_ until close(). The OS reclaims pages when
  // the fd is closed, even if the process exits abnormally.
  ::unlink(tmpl.c_str());
  ensureSpace(kInitialCapacity);
}

MmapPayloadStore::~MmapPayloadStore() {
  if (map_ != nullptr) {
    ::munmap(map_, capacity_);
  }
  if (fd_ != -1) {
    ::close(fd_);
  }
}

bool MmapPayloadStore::ensureSpace(size_t needed) {
  if (write_offset_ + needed <= capacity_) {
    return true;
  }

  size_t new_cap = (capacity_ == 0) ? kInitialCapacity : capacity_;
  while (new_cap < write_offset_ + needed) {
    new_cap *= 2;
  }

  if (::ftruncate(fd_, static_cast<off_t>(new_cap)) != 0) {
    return false;
  }

  if (map_ != nullptr) {
#if defined(__linux__)
    // mremap avoids an unmap/remap round-trip and can extend in place.
    void* m = ::mremap(map_, capacity_, new_cap, MREMAP_MAYMOVE);
    if (m == MAP_FAILED) {
      return false;
    }
    map_ = static_cast<uint8_t*>(m);
#else
    // macOS has no mremap; unmap the old region and remap at the new size.
    ::munmap(map_, capacity_);
    map_ = nullptr;
    void* m = ::mmap(nullptr, new_cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) {
      return false;
    }
    map_ = static_cast<uint8_t*>(m);
#endif
  } else {
    void* m = ::mmap(nullptr, new_cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) {
      return false;
    }
    map_ = static_cast<uint8_t*>(m);
  }

  capacity_ = new_cap;
  return true;
}

uint64_t MmapPayloadStore::appendBytes(const void* src, size_t len) {
  const uint64_t off = static_cast<uint64_t>(write_offset_);
  std::memcpy(map_ + write_offset_, src, len);
  write_offset_ += len;
  return off;
}

PayloadRef MmapPayloadStore::store(std::string data, PayloadKind /*kind*/) {
  if (data.size() <= max_inline_bytes_) {
    return PayloadRef::makeInline(std::move(data));
  }
  if (fd_ == -1 || !ensureSpace(data.size())) {
    auto buf = std::make_unique<Buffer::OwnedImpl>(data);
    return PayloadRef::makeBuffered(std::move(buf));
  }
  const uint64_t off = appendBytes(data.data(), data.size());
  return PayloadRef::makeExternal(off, data.size());
}

PayloadRef MmapPayloadStore::store(Buffer::Instance& data, PayloadKind /*kind*/) {
  const size_t len = data.length();
  if (len <= max_inline_bytes_) {
    return PayloadRef::makeInline(data.toString());
  }
  if (fd_ == -1 || !ensureSpace(len)) {
    auto buf = std::make_unique<Buffer::OwnedImpl>();
    buf->move(data);
    return PayloadRef::makeBuffered(std::move(buf));
  }
  const uint64_t off = static_cast<uint64_t>(write_offset_);
  // Walk Buffer slices directly — no contiguous intermediate copy.
  const Buffer::RawSliceVector slices = data.getRawSlices();
  for (const auto& s : slices) {
    if (s.len_ > 0) {
      std::memcpy(map_ + write_offset_, s.mem_, s.len_);
      write_offset_ += s.len_;
    }
  }
  return PayloadRef::makeExternal(off, len);
}

// ─────────────────────────────────────────────────────────────────────────────
// MmapStreamWriter
// ─────────────────────────────────────────────────────────────────────────────

MmapPayloadStore::MmapStreamWriter::MmapStreamWriter(MmapPayloadStore& store, PayloadKind /*kind*/)
    : store_(store), start_offset_(store.write_offset_) {}

void MmapPayloadStore::MmapStreamWriter::append(absl::string_view bytes) {
  if (failed_ || bytes.empty()) {
    return;
  }
  if (store_.fd_ == -1 || !store_.ensureSpace(bytes.size())) {
    failed_ = true;
    return;
  }
  store_.appendBytes(bytes.data(), bytes.size());
  total_written_ += bytes.size();
}

PayloadRef MmapPayloadStore::MmapStreamWriter::finalize() {
  if (failed_ || store_.fd_ == -1) {
    // mmap unavailable; read back whatever was written to a heap buffer.
    auto buf = std::make_unique<Buffer::OwnedImpl>();
    if (!failed_ && total_written_ > 0) {
      buf->add(store_.map_ + start_offset_, total_written_);
    }
    return PayloadRef::makeBuffered(std::move(buf));
  }
  if (total_written_ <= store_.max_inline_bytes_) {
    // Small enough to be inline: copy back from the mmap region.
    // The arena space is intentionally left allocated (not reclaimed) —
    // the waste is bounded by max_inline_bytes_ and avoids complexity.
    return PayloadRef::makeInline(
        std::string(reinterpret_cast<char*>(store_.map_ + start_offset_), total_written_));
  }
  return PayloadRef::makeExternal(static_cast<uint64_t>(start_offset_), total_written_);
}

std::unique_ptr<StreamWriter> MmapPayloadStore::beginStore(PayloadKind kind) {
  return std::make_unique<MmapStreamWriter>(*this, kind);
}

void MmapPayloadStore::fetch(const PayloadRef& ref, FetchCallback cb) {
  if (ref.empty()) {
    cb(nullptr);
    return;
  }
  if (ref.storage() == PayloadRef::Storage::External) {
    const uint64_t off = ref.externalOffset();
    const size_t   len = ref.externalLength();
    ASSERT(off + len <= write_offset_,
           "External PayloadRef references bytes beyond the store's write cursor");
    auto buf = std::make_unique<Buffer::OwnedImpl>();
    buf->add(map_ + off, len);
    cb(std::move(buf));
    return;
  }
  // Inline or Buffered — materialize via toString().
  cb(std::make_unique<Buffer::OwnedImpl>(ref.toString()));
}

void MmapPayloadStore::fetchAsync(const PayloadRef& ref, Event::Dispatcher& dispatcher,
                                  FetchCallback cb) {
  if (ref.storage() != PayloadRef::Storage::External || fd_ == -1) {
    // Non-External refs have no I/O risk; call synchronously.
    fetch(ref, std::move(cb));
    return;
  }

  // Capture by value so the thread is independent of this store's lifetime.
  // pread() uses the fd (int) directly; even if the store is destroyed and the
  // fd is closed, pread() will return -1 rather than crashing.
  const int      fd  = fd_;
  const uint64_t off = ref.externalOffset();
  const size_t   len = ref.externalLength();

  // Detached thread: performs pread (which may page-fault) off the event loop,
  // then posts the result buffer back to the dispatcher thread.
  std::thread([fd, off, len, &dispatcher, cb = std::move(cb)]() mutable {
    std::vector<char> tmp(len);
    auto buf = std::make_unique<Buffer::OwnedImpl>();
    if (len > 0 &&
        ::pread(fd, tmp.data(), len, static_cast<off_t>(off)) == static_cast<ssize_t>(len)) {
      buf->add(tmp.data(), len);
    }
    dispatcher.post([buf = std::move(buf), cb = std::move(cb)]() mutable {
      cb(std::move(buf));
    });
  }).detach();
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
