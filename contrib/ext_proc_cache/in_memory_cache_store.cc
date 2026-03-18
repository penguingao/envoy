#include "contrib/ext_proc_cache/in_memory_cache_store.h"

#include <mutex>

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

InMemoryCacheStore::InMemoryCacheStore(size_t max_entries) : max_entries_(max_entries) {}

Awaitable<std::optional<CacheLookupResult>> InMemoryCacheStore::lookup(const std::string& key) {
  std::shared_ptr<InMemoryStoredEntry> entry;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      co_return std::nullopt;
    }
    entry = it->second.entry;
    if (entry->state == EntryState::Failed) {
      co_return std::nullopt;
    }
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_it);
  }

  CacheLookupResult result;
  result.metadata = entry->metadata;
  result.body_reader = std::make_unique<InMemoryTailingBodyReader>(entry);
  co_return result;
}

Awaitable<bool> InMemoryCacheStore::store(const std::string& key, CachedEntry entry) {
  std::lock_guard<std::mutex> lock(mu_);

  auto stored = std::make_shared<InMemoryStoredEntry>();
  stored->metadata.response_headers = std::move(entry.response_headers);
  stored->metadata.trailers = std::move(entry.trailers);
  stored->metadata.response_time = entry.response_time;
  stored->metadata.status_code = entry.status_code;
  stored->metadata.content_length = entry.body.size();
  stored->body = std::move(entry.body);
  stored->state = EntryState::Complete;

  auto it = entries_.find(key);
  if (it != entries_.end()) {
    it->second.entry = std::move(stored);
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_it);
    co_return true;
  }
  while (entries_.size() >= max_entries_ && !entries_.empty()) {
    evictLru();
  }
  lru_list_.push_front(key);
  entries_[key] = MapEntry{std::move(stored), lru_list_.begin()};
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

Awaitable<bool> InMemoryCacheStore::storeHeaders(const std::string& key,
                                                  CacheEntryMetadata metadata) {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = entries_.find(key);
  if (it != entries_.end() && it->second.entry->state == EntryState::Filling) {
    co_return false; // Already being filled.
  }

  auto stored = std::make_shared<InMemoryStoredEntry>();
  stored->metadata = std::move(metadata);
  stored->state = EntryState::Filling;

  if (it != entries_.end()) {
    it->second.entry = std::move(stored);
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_it);
  } else {
    while (entries_.size() >= max_entries_ && !entries_.empty()) {
      evictLru();
    }
    lru_list_.push_front(key);
    entries_[key] = MapEntry{std::move(stored), lru_list_.begin()};
  }
  co_return true;
}

void InMemoryCacheStore::appendBody(const std::string& key, const std::string& data) {
  std::shared_ptr<InMemoryStoredEntry> entry;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      return;
    }
    entry = it->second.entry;
  }

  std::vector<std::coroutine_handle<>> to_resume;
  {
    std::lock_guard<std::mutex> lock(entry->mu);
    if (entry->state != EntryState::Filling) {
      return;
    }
    entry->body.append(data);
    to_resume.swap(entry->waiting_readers);
  }
  for (auto h : to_resume) {
    h.resume();
  }
}

void InMemoryCacheStore::finishBody(const std::string& key,
                                     std::optional<ProtoHeaderMap> trailers) {
  std::shared_ptr<InMemoryStoredEntry> entry;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      return;
    }
    entry = it->second.entry;
  }

  std::vector<std::coroutine_handle<>> to_resume;
  {
    std::lock_guard<std::mutex> lock(entry->mu);
    if (entry->state != EntryState::Filling) {
      return;
    }
    entry->state = EntryState::Complete;
    entry->metadata.content_length = entry->body.size();
    if (trailers.has_value()) {
      entry->metadata.trailers = std::move(*trailers);
    }
    to_resume.swap(entry->waiting_readers);
  }
  for (auto h : to_resume) {
    h.resume();
  }
}

void InMemoryCacheStore::abortBody(const std::string& key) {
  std::shared_ptr<InMemoryStoredEntry> entry;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      return;
    }
    entry = it->second.entry;
    lru_list_.erase(it->second.lru_it);
    entries_.erase(it);
  }

  std::vector<std::coroutine_handle<>> to_resume;
  {
    std::lock_guard<std::mutex> lock(entry->mu);
    entry->state = EntryState::Failed;
    to_resume.swap(entry->waiting_readers);
  }
  for (auto h : to_resume) {
    h.resume();
  }
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
