#pragma once

#include <coroutine>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "contrib/ext_proc_cache/cache_store.h"

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

enum class EntryState { Filling, Complete, Failed };

// Internal stored entry shared between the store and tailing readers via
// shared_ptr so the entry survives LRU eviction while readers are active.
struct InMemoryStoredEntry {
  CacheEntryMetadata metadata;
  std::string body;
  EntryState state = EntryState::Filling;

  // Coroutine handles of readers waiting for more data (Filling state only).
  std::mutex mu; // per-entry lock for body/state/waiting_readers
  std::vector<std::coroutine_handle<>> waiting_readers;

  // Resume all waiting readers (call outside entry lock).
  void resumeReaders(std::vector<std::coroutine_handle<>>& to_resume) {
    std::lock_guard<std::mutex> lock(mu);
    to_resume.swap(waiting_readers);
  }
};

// Reads body data from a stored entry, tailing the writer if the entry is
// still being filled. Suspends when caught up, resumes when data arrives.
class InMemoryTailingBodyReader : public CacheBodyReader {
public:
  explicit InMemoryTailingBodyReader(std::shared_ptr<InMemoryStoredEntry> entry)
      : entry_(std::move(entry)) {}

  ~InMemoryTailingBodyReader() override {
    // Clean up if suspended.
    if (suspended_handle_) {
      std::lock_guard<std::mutex> lock(entry_->mu);
      auto& w = entry_->waiting_readers;
      w.erase(std::remove_if(w.begin(), w.end(),
                              [this](std::coroutine_handle<> h) {
                                return h == suspended_handle_;
                              }),
              w.end());
    }
  }

  Awaitable<std::string> nextChunk(size_t max_size) override {
    while (true) {
      {
        std::lock_guard<std::mutex> lock(entry_->mu);
        if (offset_ < entry_->body.size()) {
          size_t chunk = std::min(max_size, entry_->body.size() - offset_);
          std::string result = entry_->body.substr(offset_, chunk);
          offset_ += chunk;
          co_return result;
        }
        if (entry_->state == EntryState::Complete || entry_->state == EntryState::Failed) {
          co_return std::string{};
        }
      }
      // Caught up to writer — suspend.
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
    InMemoryTailingBodyReader* reader;
    bool await_ready() const {
      std::lock_guard<std::mutex> lock(reader->entry_->mu);
      return reader->offset_ < reader->entry_->body.size() ||
             reader->entry_->state != EntryState::Filling;
    }
    void await_suspend(std::coroutine_handle<> h) {
      std::lock_guard<std::mutex> lock(reader->entry_->mu);
      reader->suspended_handle_ = h;
      reader->entry_->waiting_readers.push_back(h);
    }
    void await_resume() { reader->suspended_handle_ = nullptr; }
  };

  std::shared_ptr<InMemoryStoredEntry> entry_;
  size_t offset_ = 0;
  std::coroutine_handle<> suspended_handle_ = nullptr;
};

// Thread-safe in-memory LRU cache store with incremental body write support.
class InMemoryCacheStore : public CacheStore {
public:
  explicit InMemoryCacheStore(size_t max_entries = 1024);

  Awaitable<std::optional<CacheLookupResult>> lookup(const std::string& key) override;
  Awaitable<bool> store(const std::string& key, CachedEntry entry) override;
  Awaitable<bool> remove(const std::string& key) override;

  Awaitable<bool> storeHeaders(const std::string& key, CacheEntryMetadata metadata) override;
  void appendBody(const std::string& key, const std::string& data) override;
  void finishBody(const std::string& key,
                  std::optional<ProtoHeaderMap> trailers = std::nullopt) override;
  void abortBody(const std::string& key) override;

  size_t size() const;

private:
  void evictLru();

  mutable std::mutex mu_; // protects entries_ and lru_list_

  size_t max_entries_;
  using LruList = std::list<std::string>;
  LruList lru_list_;

  struct MapEntry {
    std::shared_ptr<InMemoryStoredEntry> entry;
    LruList::iterator lru_it;
  };
  std::unordered_map<std::string, MapEntry> entries_;
};

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
