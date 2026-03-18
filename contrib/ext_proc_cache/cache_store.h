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
  // On hit, returns metadata + a body reader for streaming body data.
  virtual Awaitable<std::optional<CacheLookupResult>> lookup(const std::string& key) = 0;

  // Store a cache entry. Returns true on success.
  virtual Awaitable<bool> store(const std::string& key, CachedEntry entry) = 0;

  // Remove a cache entry. Returns true if an entry was removed.
  virtual Awaitable<bool> remove(const std::string& key) = 0;
};

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
