#pragma once

#include "envoy/buffer/buffer.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/coroutine/task.h"
#include "source/extensions/filters/http/ai_protocol_manager/buffer_manager.h"
#include "source/extensions/filters/http/ai_protocol_manager/field_stream.h"
#include "source/extensions/filters/http/ai_protocol_manager/json_with_ext_buf.h"

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

// SinkProvider provides field streams to Serializer at the terminal sink stage.
class SinkProvider {
public:
  virtual ~SinkProvider() = default;

  // Returns true if the sink has a field stream registered for the given JSON field path.
  virtual bool hasFieldStream(absl::string_view path) const = 0;

  // Retrieves the FieldStream for the given JSON field path.
  // Returns std::nullopt if the field was dropped by the filter chain.
  virtual Coroutine::Task<absl::StatusOr<std::optional<FieldStream>>>
  getFieldStream(absl::string_view path) = 0;
};

// Serializer serializes a JsonWithExtBuf DOM into an output callback / data injector
// chunk-by-chunk without buffering large payloads into memory.
class Serializer {
public:
  using ChunkOutput = absl::AnyInvocable<void(Buffer::Instance& chunk, bool end_stream)>;

  // Streaming serialization: streams JSON chunks and offloaded/streamed fields directly
  // to the output callback without whole-payload buffering.
  static Coroutine::Task<absl::Status> serialize(const JsonWithExtBuf& doc,
                                                 BufferManager* buffer_manager, ChunkOutput output,
                                                 SinkProvider* sink_provider = nullptr);

  // Synchronous convenience serializer for tests / small non-streamed docs.
  static absl::StatusOr<Buffer::OwnedImpl> serialize(const JsonWithExtBuf& doc,
                                                     BufferManager* buffer_manager);

private:
  static Coroutine::Task<absl::Status>
  serializeNode(const nlohmann::json& node, absl::string_view current_path,
                BufferManager* buffer_manager, ChunkOutput& output, Buffer::OwnedImpl& small_buf,
                SinkProvider* sink_provider);
};

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
