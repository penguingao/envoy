#pragma once

#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "contrib/ext_proc_cache/cache_store.h"

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

// Reads body data from an in-memory shared string.
class InMemoryBodyReader : public CacheBodyReader {
public:
  explicit InMemoryBodyReader(std::shared_ptr<const std::string> body) : body_(std::move(body)) {}

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

// Factory that produces InMemoryBodyReaders from a shared body string.
class InMemoryBodyReaderFactory : public CacheBodyReaderFactory {
public:
  explicit InMemoryBodyReaderFactory(std::shared_ptr<const std::string> body)
      : body_(std::move(body)) {}

  std::unique_ptr<CacheBodyReader> createReader() override {
    return std::make_unique<InMemoryBodyReader>(body_);
  }

private:
  std::shared_ptr<const std::string> body_;
};

// Thread-safe in-memory LRU cache store. All Awaitables complete synchronously.
class InMemoryCacheStore : public CacheStore {
public:
  explicit InMemoryCacheStore(size_t max_entries = 1024);

  Awaitable<std::optional<CacheLookupResult>> lookup(const std::string& key) override;
  Awaitable<bool> store(const std::string& key, CachedEntry entry) override;
  Awaitable<bool> remove(const std::string& key) override;

  std::shared_ptr<CacheBodyReaderFactory>
  createBodyReaderFactory(std::string body) override;

  // Returns the current number of entries. Thread-safe.
  size_t size() const;

private:
  // Evicts the least recently used entry. Caller must hold mu_.
  void evictLru();

  mutable std::mutex mu_;
  size_t max_entries_;

  // LRU list: front = most recently used, back = least recently used.
  using LruList = std::list<std::string>;
  LruList lru_list_;

  struct StoredEntry {
    CacheEntryMetadata metadata;
    std::shared_ptr<const std::string> body;
    LruList::iterator lru_it;
  };
  std::unordered_map<std::string, StoredEntry> entries_;
};

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
