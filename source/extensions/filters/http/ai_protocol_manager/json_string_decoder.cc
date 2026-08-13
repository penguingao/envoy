#include "source/extensions/filters/http/ai_protocol_manager/json_string_decoder.h"

#include <utility>

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

namespace {
// The synthetic document's delimiters. The caller's bytes are the content
// between them.
constexpr absl::string_view Quote = "\"";
} // namespace

JsonStringDecoder::JsonStringDecoder(DecodedCallback on_decoded)
    : on_decoded_(std::move(on_decoded)), cursor_(*this, /*track_paths=*/false) {}

absl::Status JsonStringDecoder::feed(absl::string_view chunk, bool end) {
  // Check the latched error before the finished check: a decoder that failed
  // reports why it failed, not that it is done.
  if (!status_.ok()) {
    return status_;
  }
  if (finished_) {
    return absl::FailedPreconditionError("ai json string: feed() called after the value completed");
  }

  if (!opened_) {
    opened_ = true;
    if (absl::Status status = cursor_.feed(Quote, /*closed=*/false); !status.ok()) {
      setError(std::move(status));
      return status_;
    }
  }

  if (!chunk.empty()) {
    if (absl::Status status = cursor_.feed(chunk, /*closed=*/false); !status.ok()) {
      setError(std::move(status));
      return status_;
    }
    // A callback may have failed the decode without the cursor itself erroring.
    if (!status_.ok()) {
      return status_;
    }
  }

  if (!end) {
    return absl::OkStatus();
  }

  // Closing the document is what rejects content the cursor was still willing to
  // extend: a trailing backslash, or a \u escape cut short.
  if (absl::Status status = cursor_.feed(Quote, /*closed=*/true); !status.ok()) {
    setError(std::move(status));
    return status_;
  }
  if (!status_.ok()) {
    return status_;
  }
  finished_ = true;
  return absl::OkStatus();
}

bool JsonStringDecoder::openStringCapture(absl::string_view, int, size_t) {
  // Capture the one string this document contains.
  return true;
}

bool JsonStringDecoder::onStringChunk(absl::string_view, int, absl::string_view chunk) {
  on_decoded_(chunk);
  return true;
}

void JsonStringDecoder::closeStringCapture(absl::string_view, int, size_t) {}

absl::Status JsonStringDecoder::onKey(absl::string_view, int, size_t) {
  notAString();
  return status_;
}

absl::Status JsonStringDecoder::onNumber(absl::string_view, absl::string_view, int, size_t,
                                         size_t) {
  notAString();
  return status_;
}

absl::Status JsonStringDecoder::onBoolean(absl::string_view, bool, int, size_t, size_t) {
  notAString();
  return status_;
}

void JsonStringDecoder::onNull(absl::string_view, int, size_t, size_t) { notAString(); }

void JsonStringDecoder::onContainerOpen(absl::string_view, bool, int, size_t) { notAString(); }

void JsonStringDecoder::onContainerClose(int, size_t) { notAString(); }

void JsonStringDecoder::notAString() {
  setError(absl::InvalidArgumentError("ai json string: value is not a plain string"));
}

void JsonStringDecoder::setError(absl::Status status) {
  if (status_.ok()) {
    status_ = std::move(status);
  }
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
