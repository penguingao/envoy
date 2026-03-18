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

// Thread-safe in-memory LRU cache store. All Awaitables complete synchronously.
class InMemoryCacheStore : public CacheStore {
public:
  explicit InMemoryCacheStore(size_t max_entries = 1024);

  Awaitable<std::optional<CacheLookupResult>> lookup(const std::string& key) override;
  Awaitable<bool> store(const std::string& key, CachedEntry entry) override;
  Awaitable<bool> remove(const std::string& key) override;

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
