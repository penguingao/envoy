#pragma once

#include <memory>
#include <optional>
#include <string>

#include "contrib/ext_proc_cache/awaitable.h"
#include "contrib/ext_proc_cache/cache_types.h"

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

// Abstract interface for a cache storage backend. All methods are coroutine-native.
class CacheStore {
public:
  virtual ~CacheStore() = default;

  // Look up a cache entry by key. Returns nullopt on miss.
  // On hit, returns metadata + a body reader. For in-progress entries (being
  // filled), the reader tails the writer — suspending when caught up and
  // resuming when more data arrives.
  virtual Awaitable<std::optional<CacheLookupResult>> lookup(const std::string& key) = 0;

  // Store a complete cache entry atomically. Returns true on success.
  // Used for 304 revalidation (re-storing a refreshed entry).
  virtual Awaitable<bool> store(const std::string& key, CachedEntry entry) = 0;

  // Remove a cache entry. Returns true if an entry was removed.
  virtual Awaitable<bool> remove(const std::string& key) = 0;

  // --- Incremental body write API ---

  // Commit metadata with empty body, marking the entry as "filling".
  virtual Awaitable<bool> storeHeaders(const std::string& key, CacheEntryMetadata metadata) = 0;

  // Append a body chunk to an in-progress entry. Resumes tailing readers.
  virtual void appendBody(const std::string& key, const std::string& data) = 0;

  // Mark the entry as complete. Sets final content_length. Resumes readers.
  virtual void finishBody(const std::string& key,
                          std::optional<ProtoHeaderMap> trailers = std::nullopt) = 0;

  // Abort an in-progress fill. Removes the entry and signals error to readers.
  virtual void abortBody(const std::string& key) = 0;
};

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
