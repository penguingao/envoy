#pragma once

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "envoy/config/core/v3/base.pb.h"

#include "contrib/ext_proc_cache/awaitable.h"

#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

// Time types used throughout.
using SystemTime = std::chrono::system_clock::time_point;
using Seconds = std::chrono::seconds;
using OptionalDuration = std::optional<SystemTime::duration>;

// Alias for the proto header type used throughout.
using ProtoHeaderMap = envoy::config::core::v3::HeaderMap;

// A complete cached HTTP response used for the write/store path.
struct CachedEntry {
  ProtoHeaderMap response_headers;
  std::string body;
  std::optional<ProtoHeaderMap> trailers;
  SystemTime response_time; // when this was stored
  uint32_t status_code = 0; // extracted from :status
};

// Cached entry metadata — cheap to copy, no body data.
struct CacheEntryMetadata {
  ProtoHeaderMap response_headers;
  std::optional<ProtoHeaderMap> trailers;
  SystemTime response_time;
  uint32_t status_code = 0;
  uint64_t content_length = 0;
};

// Abstract interface for reading cached body data chunk by chunk.
// Each reader has its own read position and is used by a single consumer.
class CacheBodyReader {
public:
  virtual ~CacheBodyReader() = default;

  // Returns the next chunk of body data up to max_size bytes.
  // Returns empty string when exhausted.
  virtual Awaitable<std::string> nextChunk(size_t max_size) = 0;

  // Read all remaining body data into a single string.
  virtual Awaitable<std::string> readAll() = 0;
};

// Reads body data from a shared in-memory string. General-purpose reader used
// by the coordinator to distribute body data to coalesced waiters, and by the
// in-memory store for cache hits.
class StringBodyReader : public CacheBodyReader {
public:
  explicit StringBodyReader(std::shared_ptr<const std::string> body) : body_(std::move(body)) {}

  Awaitable<std::string> nextChunk(size_t max_size) override {
    if (offset_ >= body_->size()) {
      co_return std::string{};
    }
    size_t chunk = std::min(max_size, body_->size() - offset_);
    std::string result = body_->substr(offset_, chunk);
    offset_ += chunk;
    co_return result;
  }

  Awaitable<std::string> readAll() override {
    std::string result = body_->substr(offset_);
    offset_ = body_->size();
    co_return result;
  }

private:
  std::shared_ptr<const std::string> body_;
  size_t offset_ = 0;
};

// Shared append-only body buffer written by a cache filler and read by
// multiple FollowingBodyReaders at independent offsets. When a reader catches
// up to the write frontier, its coroutine suspends until the writer appends
// more data or signals completion.
class SharedBodyStream {
public:
  // Called by the filler to append body data. Resumes any waiting readers.
  void append(const std::string& data) {
    std::vector<std::coroutine_handle<>> to_resume;
    {
      std::lock_guard<std::mutex> lock(mu_);
      buffer_.append(data);
      to_resume.swap(waiting_readers_);
    }
    for (auto h : to_resume) {
      h.resume();
    }
  }

  // Called by the filler when the body is complete.
  void finish() {
    std::vector<std::coroutine_handle<>> to_resume;
    {
      std::lock_guard<std::mutex> lock(mu_);
      done_ = true;
      to_resume.swap(waiting_readers_);
    }
    for (auto h : to_resume) {
      h.resume();
    }
  }

  // Called when the filler fails mid-stream.
  void signalError() {
    std::vector<std::coroutine_handle<>> to_resume;
    {
      std::lock_guard<std::mutex> lock(mu_);
      error_ = true;
      done_ = true;
      to_resume.swap(waiting_readers_);
    }
    for (auto h : to_resume) {
      h.resume();
    }
  }

  // Read up to max_size bytes starting at offset. Called under no lock by
  // FollowingBodyReader after checking availability.
  std::string read(size_t offset, size_t max_size) {
    std::lock_guard<std::mutex> lock(mu_);
    if (offset >= buffer_.size()) {
      return {};
    }
    size_t chunk = std::min(max_size, buffer_.size() - offset);
    return buffer_.substr(offset, chunk);
  }

  size_t available() {
    std::lock_guard<std::mutex> lock(mu_);
    return buffer_.size();
  }

  bool isDone() {
    std::lock_guard<std::mutex> lock(mu_);
    return done_;
  }

  bool hasError() {
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
  }

  // Register a reader coroutine to be resumed when data arrives.
  void suspendReader(std::coroutine_handle<> h) {
    std::lock_guard<std::mutex> lock(mu_);
    waiting_readers_.push_back(h);
  }

  // Remove a reader handle (e.g. on cancellation). Returns true if found.
  bool removeReader(std::coroutine_handle<> h) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(waiting_readers_.begin(), waiting_readers_.end(),
                           [h](std::coroutine_handle<> w) { return w == h; });
    if (it != waiting_readers_.end()) {
      waiting_readers_.erase(it);
      return true;
    }
    return false;
  }

private:
  std::mutex mu_;
  std::string buffer_;
  bool done_ = false;
  bool error_ = false;
  std::vector<std::coroutine_handle<>> waiting_readers_;
};

// A CacheBodyReader that tails a SharedBodyStream being written by a filler.
// Suspends when caught up and resumes when the filler appends more data.
class FollowingBodyReader : public CacheBodyReader {
public:
  explicit FollowingBodyReader(std::shared_ptr<SharedBodyStream> stream)
      : stream_(std::move(stream)) {}

  ~FollowingBodyReader() override {
    // Clean up if we're suspended waiting for data.
    if (suspended_handle_) {
      stream_->removeReader(suspended_handle_);
    }
  }

  Awaitable<std::string> nextChunk(size_t max_size) override {
    while (true) {
      // Check if data is available.
      size_t avail = stream_->available();
      if (offset_ < avail) {
        auto data = stream_->read(offset_, max_size);
        offset_ += data.size();
        co_return data;
      }

      // No more data and stream is done.
      if (stream_->isDone()) {
        co_return std::string{};
      }

      // Caught up — suspend until writer appends or finishes.
      co_await WaitForData{this};
    }
  }

  Awaitable<std::string> readAll() override {
    std::string result;
    while (true) {
      auto chunk = co_await nextChunk(64 * 1024);
      if (chunk.empty()) {
        break;
      }
      result.append(chunk);
    }
    co_return result;
  }

private:
  struct WaitForData {
    FollowingBodyReader* reader;
    bool await_ready() const {
      // Ready if data available or stream done.
      return reader->offset_ < reader->stream_->available() ||
             reader->stream_->isDone();
    }
    void await_suspend(std::coroutine_handle<> h) {
      reader->suspended_handle_ = h;
      reader->stream_->suspendReader(h);
    }
    void await_resume() { reader->suspended_handle_ = nullptr; }
  };

  std::shared_ptr<SharedBodyStream> stream_;
  size_t offset_ = 0;
  std::coroutine_handle<> suspended_handle_ = nullptr;
};

// Result of a cache store lookup (metadata + body reader).
struct CacheLookupResult {
  CacheEntryMetadata metadata;
  std::unique_ptr<CacheBodyReader> body_reader;
};

// Result of a coordinated lookup.
enum class LookupStatus {
  Hit,       // entry is populated, serve from cache
  YouFill,   // you are the filler — proceed to upstream, then store
  TimedOut,  // your deadline expired while waiting
  Cancelled, // your stream was cancelled while waiting
};

struct CoordinatedLookupResult {
  LookupStatus status;
  std::optional<CacheEntryMetadata> metadata;         // present when status == Hit
  std::unique_ptr<CacheBodyReader> body_reader;        // present when status == Hit
};

// Whether a given cache entry is good for the current request.
enum class CacheEntryStatus {
  Ok,                  // fresh, appropriate response
  Unusable,            // no usable entry
  RequiresValidation,  // stale but can be validated
};

// Contains information about whether a cache entry is usable.
struct CacheEntryUsability {
  CacheEntryStatus status = CacheEntryStatus::Unusable;
  Seconds age = Seconds::max();
  Seconds ttl = Seconds::max();

  friend bool operator==(const CacheEntryUsability& a, const CacheEntryUsability& b) {
    return std::tie(a.status, a.age, a.ttl) == std::tie(b.status, b.age, b.ttl);
  }
};

// Request cacheability decision.
enum class RequestCacheability {
  Cacheable,
  Bypass,
  NoStore,
};

// Response cacheability decision.
enum class ResponseCacheability {
  DoNotStore,
  StoreFullResponse,
};

// Cache-Control directives from a request.
struct RequestCacheControl {
  RequestCacheControl() = default;
  explicit RequestCacheControl(absl::string_view cache_control_header);

  bool must_validate_ = false;
  bool no_store_ = false;
  bool no_transform_ = false;
  bool only_if_cached_ = false;
  OptionalDuration max_age_;
  OptionalDuration min_fresh_;
  OptionalDuration max_stale_;
};

// Cache-Control directives from a response.
struct ResponseCacheControl {
  ResponseCacheControl() = default;
  explicit ResponseCacheControl(absl::string_view cache_control_header);

  bool must_validate_ = false;
  bool no_store_ = false;
  bool no_transform_ = false;
  bool no_stale_ = false;
  bool is_public_ = false;
  OptionalDuration max_age_;
};

// --- Proto header helpers ---

// Get the value of a header by name. Returns empty string if not found.
std::string getHeader(const ProtoHeaderMap& headers, absl::string_view name);

// Check if a header exists.
bool hasHeader(const ProtoHeaderMap& headers, absl::string_view name);

// Set a header value, replacing any existing value with that name.
void setHeader(ProtoHeaderMap& headers, const std::string& name, const std::string& value);

// Remove a header by name.
void removeHeader(ProtoHeaderMap& headers, absl::string_view name);

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
