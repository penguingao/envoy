#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"

#include "absl/strings/string_view.h"
#include "nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// A parsed inference request: the payload DOM plus the accessors an extension
// needs to act on it without knowing the JSON layout.
//
// Adopts a parsed document by move, as json_with_ext_buf.h anticipates. It adds
// two things over the raw DOM:
//
// Modification tracking. A payload nobody changed is forwarded by replaying its
// original bytes, which is both cheaper than serializing and the only way to
// forward it byte-identically. That fast path is only sound if "nobody changed
// it" is trustworthy, so reaching the DOM mutably goes through mutableJson(),
// which sets the flag. The flag is coarse -- taking the mutable reference marks
// the payload dirty whether or not anything was written -- because a false
// positive only costs the slow path while a false negative would silently
// forward stale bytes.
//
// Offload accounting. An offloaded value's size is in its ExternalRef, so a
// filter can weigh a prompt without reading any of it back.
//
// Accessors return nullopt for a field that is absent or of an unexpected type
// rather than failing: validating the payload against its schema is the schema
// layer's job, and a filter that runs after it should not have to re-check.
class InferenceRequest {
public:
  explicit InferenceRequest(JsonWithExtBuf payload) : payload_(std::move(payload)) {}

  // Move-only, following JsonWithExtBuf: copying a payload would defeat
  // offloading.
  InferenceRequest(InferenceRequest&&) = default;
  InferenceRequest& operator=(InferenceRequest&&) = default;
  InferenceRequest(const InferenceRequest&) = delete;
  InferenceRequest& operator=(const InferenceRequest&) = delete;

  const nlohmann::json& json() const { return payload_.json(); }

  // Marks the payload modified. Use json() to read.
  nlohmann::json& mutableJson() {
    dirty_ = true;
    return payload_.json();
  }

  // True once mutableJson() or a setter has been called.
  bool dirty() const { return dirty_; }

  // Releases the document, ending this wrapper's tracking of it.
  JsonWithExtBuf release() && { return std::move(payload_); }

  // Common fields.
  std::optional<absl::string_view> model() const;
  std::optional<bool> stream() const;
  std::optional<std::int64_t> maxTokens() const;

  // The "messages" / "tools" arrays, or nullptr when absent or not an array.
  const nlohmann::json* messages() const;
  const nlohmann::json* tools() const;

  // Replaces "model", marking the payload modified. This is how a router sends
  // a request to a different model than the caller named.
  void setModel(absl::string_view model);

  // Every offloaded value in the payload, in document order.
  std::vector<JsonWithExtBuf::ExternalRef> offloadedRanges() const;

  // Total size of the offloaded values. Read from the references, so it costs
  // nothing and needs no buffer -- which is what lets a filter apply a size
  // policy to a prompt it has not read.
  std::uint64_t offloadedBytes() const;

private:
  // Returns the named member of the root object, or nullptr.
  const nlohmann::json* member(absl::string_view name) const;

  JsonWithExtBuf payload_;
  bool dirty_{false};
};

// Deleter that lets an owner reclaim a request instead of destroying it.
//
// A filter that returns without forwarding must not strand the payload -- the
// chain forwards it on the filter's behalf. The only place that can be noticed
// is the moment the filter's unique_ptr goes out of scope, so the reclaim hook
// lives here. Default-constructed it simply deletes, which is what every owner
// outside the chain wants.
class InferenceRequestDisposer {
public:
  using DropCallback = std::function<void(InferenceRequest*)>;

  InferenceRequestDisposer() = default;
  explicit InferenceRequestDisposer(DropCallback on_drop) : on_drop_(std::move(on_drop)) {}

  void operator()(InferenceRequest* request) const {
    if (on_drop_ != nullptr) {
      on_drop_(request);
      return;
    }
    delete request;
  }

private:
  DropCallback on_drop_;
};

using InferenceRequestPtr = std::unique_ptr<InferenceRequest, InferenceRequestDisposer>;

// Builds an owning pointer that just deletes, for callers with no chain behind
// them (tests, and the filter before it hands the payload over).
inline InferenceRequestPtr makeInferenceRequest(JsonWithExtBuf payload) {
  return InferenceRequestPtr(new InferenceRequest(std::move(payload)), InferenceRequestDisposer());
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
