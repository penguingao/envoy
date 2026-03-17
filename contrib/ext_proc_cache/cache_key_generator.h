#pragma once

#include <string>

#include "contrib/ext_proc_cache/cache_types.h"

#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

// Abstract interface for computing cache keys from request headers.
class CacheKeyGenerator {
public:
  virtual ~CacheKeyGenerator() = default;
  virtual std::string generateKey(const ProtoHeaderMap& request_headers) = 0;
};

// Default key generator: "{scheme}://{host}{path}" from pseudo-headers.
class DefaultCacheKeyGenerator : public CacheKeyGenerator {
public:
  std::string generateKey(const ProtoHeaderMap& request_headers) override {
    const std::string scheme = getHeader(request_headers, ":scheme");
    const std::string host = getHeader(request_headers, ":authority");
    const std::string path = getHeader(request_headers, ":path");
    return absl::StrCat(scheme, "://", host, path);
  }
};

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
