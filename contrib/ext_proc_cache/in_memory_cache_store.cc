#include "contrib/ext_proc_cache/in_memory_cache_store.h"

#include <mutex>

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

InMemoryCacheStore::InMemoryCacheStore(size_t max_entries) : max_entries_(max_entries) {}

Awaitable<std::optional<CachedEntry>> InMemoryCacheStore::lookup(const std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    co_return std::nullopt;
  }
  // Move to front of LRU list.
  lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_it);
  co_return it->second.cached;
}

Awaitable<bool> InMemoryCacheStore::store(const std::string& key, CachedEntry entry) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = entries_.find(key);
  if (it != entries_.end()) {
    // Update existing entry.
    it->second.cached = std::move(entry);
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_it);
    co_return true;
  }
  // Evict if at capacity.
  while (entries_.size() >= max_entries_ && !entries_.empty()) {
    evictLru();
  }
  // Insert new entry.
  lru_list_.push_front(key);
  entries_[key] = StoredEntry{std::move(entry), lru_list_.begin()};
  co_return true;
}

Awaitable<bool> InMemoryCacheStore::remove(const std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    co_return false;
  }
  lru_list_.erase(it->second.lru_it);
  entries_.erase(it);
  co_return true;
}

size_t InMemoryCacheStore::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return entries_.size();
}

void InMemoryCacheStore::evictLru() {
  if (lru_list_.empty()) {
    return;
  }
  const std::string& oldest_key = lru_list_.back();
  entries_.erase(oldest_key);
  lru_list_.pop_back();
}

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
