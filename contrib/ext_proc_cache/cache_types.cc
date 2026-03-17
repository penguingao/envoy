#include "contrib/ext_proc_cache/cache_types.h"

#include <vector>

#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

// --- Proto header helpers ---

std::string getHeader(const ProtoHeaderMap& headers, absl::string_view name) {
  for (const auto& header : headers.headers()) {
    if (header.key() == name) {
      // The ext_proc filter populates raw_value (bytes) rather than value (string).
      if (!header.raw_value().empty()) {
        return header.raw_value();
      }
      return header.value();
    }
  }
  return "";
}

bool hasHeader(const ProtoHeaderMap& headers, absl::string_view name) {
  for (const auto& header : headers.headers()) {
    if (header.key() == name) {
      return true;
    }
  }
  return false;
}

void setHeader(ProtoHeaderMap& headers, const std::string& name, const std::string& value) {
  for (auto& header : *headers.mutable_headers()) {
    if (header.key() == name) {
      header.set_value(value);
      return;
    }
  }
  auto* new_header = headers.add_headers();
  new_header->set_key(name);
  new_header->set_value(value);
}

void removeHeader(ProtoHeaderMap& headers, absl::string_view name) {
  auto* mutable_headers = headers.mutable_headers();
  for (int i = mutable_headers->size() - 1; i >= 0; --i) {
    if (mutable_headers->Get(i).key() == name) {
      mutable_headers->DeleteSubrange(i, 1);
    }
  }
}

// --- Cache-Control parsing ---

namespace {

OptionalDuration parseDuration(absl::string_view s) {
  if (s.size() > 1 && s.front() == '"' && s.back() == '"') {
    s = s.substr(1, s.size() - 2);
  }
  long num;
  if (absl::SimpleAtoi(s, &num) && num >= 0) {
    return Seconds(num);
  }
  return std::nullopt;
}

std::pair<absl::string_view, absl::string_view>
separateDirectiveAndArgument(absl::string_view full_directive) {
  return absl::StrSplit(absl::StripAsciiWhitespace(full_directive), absl::MaxSplits('=', 1));
}

} // namespace

RequestCacheControl::RequestCacheControl(absl::string_view cache_control_header) {
  const std::vector<absl::string_view> directives = absl::StrSplit(cache_control_header, ',');
  for (auto full_directive : directives) {
    auto [directive, argument] = separateDirectiveAndArgument(full_directive);
    if (directive == "no-cache") {
      must_validate_ = true;
    } else if (directive == "no-store") {
      no_store_ = true;
    } else if (directive == "no-transform") {
      no_transform_ = true;
    } else if (directive == "only-if-cached") {
      only_if_cached_ = true;
    } else if (directive == "max-age") {
      max_age_ = parseDuration(argument);
    } else if (directive == "min-fresh") {
      min_fresh_ = parseDuration(argument);
    } else if (directive == "max-stale") {
      max_stale_ = argument.empty() ? SystemTime::duration::max() : parseDuration(argument);
    }
  }
}

ResponseCacheControl::ResponseCacheControl(absl::string_view cache_control_header) {
  const std::vector<absl::string_view> directives = absl::StrSplit(cache_control_header, ',');
  for (auto full_directive : directives) {
    auto [directive, argument] = separateDirectiveAndArgument(full_directive);
    if (directive == "no-cache") {
      must_validate_ = true;
    } else if (directive == "must-revalidate" || directive == "proxy-revalidate") {
      no_stale_ = true;
    } else if (directive == "no-store" || directive == "private") {
      no_store_ = true;
    } else if (directive == "no-transform") {
      no_transform_ = true;
    } else if (directive == "public") {
      is_public_ = true;
    } else if (directive == "s-maxage") {
      max_age_ = parseDuration(argument);
    } else if (!max_age_.has_value() && directive == "max-age") {
      max_age_ = parseDuration(argument);
    }
  }
}

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
