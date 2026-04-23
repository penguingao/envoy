#pragma once

#include <functional>
#include <memory>
#include <string>

#include "envoy/buffer/buffer.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"

#include "absl/strings/string_view.h"

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
  enum class Storage { Inline, Buffered };

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

  size_t size() const {
    return storage_ == Storage::Inline ? inline_data_.size()
                                       : (buffered_data_ ? buffered_data_->length() : 0);
  }

  bool empty() const { return size() == 0; }

  // Materializes the value into a std::string regardless of storage type.
  std::string toString() const;

private:
  Storage              storage_{Storage::Inline};
  std::string          inline_data_;
  Buffer::InstancePtr  buffered_data_;
};

using FetchCallback = std::function<void(Buffer::InstancePtr)>;

// PayloadStore manages the lifecycle of large field values.
class PayloadStore {
public:
  virtual ~PayloadStore() = default;

  virtual PayloadRef store(std::string data, PayloadKind kind) = 0;
  virtual PayloadRef store(Buffer::Instance& data, PayloadKind kind) = 0;

  // Materializes a ref back into a Buffer::Instance via callback (may be
  // synchronous for Inline/Buffered; async for External backends).
  virtual void fetch(const PayloadRef& ref, FetchCallback cb) = 0;
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
