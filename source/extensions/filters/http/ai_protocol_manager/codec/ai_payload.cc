#include "source/extensions/filters/http/ai_protocol_manager/codec/ai_payload.h"

#include <memory>

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {

namespace {

class InMemoryStreamWriter : public StreamWriter {
public:
  InMemoryStreamWriter(InMemoryPayloadStore& store, PayloadKind kind)
      : store_(store), kind_(kind) {}

  void append(absl::string_view bytes) override {
    buf_.add(bytes.data(), bytes.size());
  }

  PayloadRef finalize() override {
    return store_.store(buf_, kind_);
  }

private:
  InMemoryPayloadStore& store_;
  PayloadKind           kind_;
  Buffer::OwnedImpl     buf_;
};

} // namespace

std::unique_ptr<StreamWriter> InMemoryPayloadStore::beginStore(PayloadKind kind) {
  return std::make_unique<InMemoryStreamWriter>(*this, kind);
}

std::string PayloadRef::toString() const {
  switch (storage_) {
  case Storage::Inline:
    return inline_data_;
  case Storage::Buffered:
    if (buffered_data_ && buffered_data_->length() > 0) {
      return buffered_data_->toString();
    }
    return {};
  case Storage::External:
    PANIC("External PayloadRef must be materialized through PayloadStore::fetch()");
  }
  return {};
}

} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
