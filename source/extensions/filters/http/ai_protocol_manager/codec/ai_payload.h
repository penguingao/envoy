#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "envoy/buffer/buffer.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"

#include "absl/strings/string_view.h"

// Forward declaration — avoids pulling dispatcher.h into every translation unit
// that includes ai_payload.h. Callers of fetchAsync must include the full header.
namespace Envoy {
namespace Event {
class Dispatcher;
} // namespace Event
} // namespace Envoy

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

enum class PayloadKind { JsonObject, JsonArray, Binary };

// PayloadRef is a lightweight handle to a potentially large field value.
// Fields below the configured byte threshold are kept Inline (zero-copy string
// view semantics); fields at or above the threshold are kept Buffered behind a
// Buffer::Instance so they never live fully in filter memory simultaneously.
class PayloadRef {
public:
  enum class Storage { Inline, Buffered, External };

  PayloadRef() = default;

  static PayloadRef makeInline(std::string data) {
    PayloadRef ref;
    ref.storage_     = Storage::Inline;
    ref.inline_data_ = std::move(data);
    return ref;
  }

  static PayloadRef makeBuffered(Buffer::InstancePtr data) {
    PayloadRef ref;
    ref.storage_       = Storage::Buffered;
    ref.buffered_data_ = std::move(data);
    return ref;
  }

  // Creates a ref to a byte range inside an external store's backing file.
  // offset and length are byte positions within the store's mmap region.
  static PayloadRef makeExternal(uint64_t offset, size_t length) {
    PayloadRef ref;
    ref.storage_          = Storage::External;
    ref.external_offset_  = offset;
    ref.external_length_  = length;
    return ref;
  }

  Storage storage() const { return storage_; }

  absl::string_view inlineView() const {
    ASSERT(storage_ == Storage::Inline);
    return inline_data_;
  }

  const Buffer::Instance& buffered() const {
    ASSERT(storage_ == Storage::Buffered);
    ASSERT(buffered_data_ != nullptr);
    return *buffered_data_;
  }

  uint64_t externalOffset() const {
    ASSERT(storage_ == Storage::External);
    return external_offset_;
  }

  size_t externalLength() const {
    ASSERT(storage_ == Storage::External);
    return external_length_;
  }

  size_t size() const {
    switch (storage_) {
    case Storage::Inline:   return inline_data_.size();
    case Storage::Buffered: return buffered_data_ ? buffered_data_->length() : 0;
    case Storage::External: return external_length_;
    }
    return 0;
  }

  bool empty() const { return size() == 0; }

  // Creates an independent copy of this ref.
  // External: new handle to the same byte range (zero extra allocation).
  // Inline:   copies the string.
  // Buffered: copies the buffer contents.
  PayloadRef clone() const {
    switch (storage_) {
    case Storage::Inline:
      return PayloadRef::makeInline(inline_data_);
    case Storage::External:
      return PayloadRef::makeExternal(external_offset_, external_length_);
    case Storage::Buffered: {
      auto buf = std::make_unique<Buffer::OwnedImpl>();
      if (buffered_data_) buf->add(*buffered_data_);
      return PayloadRef::makeBuffered(std::move(buf));
    }
    }
    return {};
  }

  // Materializes the value into a std::string. Valid only for Inline and Buffered;
  // External refs must be fetched through PayloadStore::fetch().
  std::string toString() const;

private:
  Storage             storage_{Storage::Inline};
  std::string         inline_data_;
  Buffer::InstancePtr buffered_data_;
  uint64_t            external_offset_{0};
  size_t              external_length_{0};
};

using FetchCallback = std::function<void(Buffer::InstancePtr)>;

// StreamWriter allows bytes to be appended to a PayloadStore entry
// incrementally rather than supplying the full content upfront. Call
// append() for each chunk, then finalize() once to obtain the PayloadRef.
// A StreamWriter instance must not outlive the PayloadStore that created it.
class StreamWriter {
public:
  virtual ~StreamWriter() = default;
  virtual void append(absl::string_view bytes) = 0;
  virtual PayloadRef finalize() = 0;
};

// PayloadStore manages the lifecycle of large field values.
class PayloadStore {
public:
  virtual ~PayloadStore() = default;

  virtual PayloadRef store(std::string data, PayloadKind kind) = 0;
  virtual PayloadRef store(Buffer::Instance& data, PayloadKind kind) = 0;

  // Open a streaming write session. append() the bytes as they arrive,
  // then call finalize() to commit and obtain the PayloadRef.
  virtual std::unique_ptr<StreamWriter> beginStore(PayloadKind kind) = 0;

  // Materializes a ref back into a Buffer::Instance via callback (may be
  // synchronous for Inline/Buffered; async for External backends).
  virtual void fetch(const PayloadRef& ref, FetchCallback cb) = 0;

  // Asynchronous variant. For External refs, implementations may schedule the
  // read on a background thread and invoke `cb` on the event loop thread via
  // `dispatcher.post()`; for Inline/Buffered the default calls fetch() inline.
  // The callback is always invoked exactly once, on the dispatcher thread.
  virtual void fetchAsync(const PayloadRef& ref, Event::Dispatcher& /*dispatcher*/,
                          FetchCallback cb) {
    fetch(ref, std::move(cb));
  }
};

// Default in-memory implementation. Fields whose serialized size is at or
// below max_inline_bytes are stored as Inline refs; larger fields are stored
// as Buffered refs. All fetches are synchronous.
class InMemoryPayloadStore : public PayloadStore {
public:
  static constexpr size_t kDefaultMaxInlineBytes = 4096;

  explicit InMemoryPayloadStore(size_t max_inline_bytes = kDefaultMaxInlineBytes)
      : max_inline_bytes_(max_inline_bytes) {}

  PayloadRef store(std::string data, PayloadKind /*kind*/) override {
    if (data.size() <= max_inline_bytes_) {
      return PayloadRef::makeInline(std::move(data));
    }
    auto buf = std::make_unique<Buffer::OwnedImpl>(data);
    return PayloadRef::makeBuffered(std::move(buf));
  }

  PayloadRef store(Buffer::Instance& data, PayloadKind /*kind*/) override {
    if (data.length() <= max_inline_bytes_) {
      return PayloadRef::makeInline(data.toString());
    }
    auto buf = std::make_unique<Buffer::OwnedImpl>();
    buf->move(data);
    return PayloadRef::makeBuffered(std::move(buf));
  }

  std::unique_ptr<StreamWriter> beginStore(PayloadKind kind) override;

  void fetch(const PayloadRef& ref, FetchCallback cb) override {
    if (ref.empty()) {
      cb(nullptr);
      return;
    }
    cb(std::make_unique<Buffer::OwnedImpl>(ref.toString()));
  }

private:
  size_t max_inline_bytes_;
};

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
