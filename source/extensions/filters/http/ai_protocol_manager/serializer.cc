#include "source/extensions/filters/http/ai_protocol_manager/serializer.h"

#include <algorithm>
#include <string>
#include <utility>

#include "source/common/coroutine/status_macros.h"

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {

namespace {

absl::Status serializeNodeSync(const nlohmann::json& node, BufferManager* buffer_manager,
                               Buffer::OwnedImpl& output) {
  if (JsonWithExtBuf::isExternalRef(node)) {
    auto ref_or = JsonWithExtBuf::externalRef(node);
    if (!ref_or.ok()) {
      return ref_or.status();
    }
    const auto& ref = *ref_or;
    output.add("\"");
    if (ref.length > 0) {
      if (buffer_manager == nullptr) {
        return absl::InternalError("buffer_manager is null for ExternalRef node");
      }
      bool read_done = false;
      absl::Status read_status = absl::OkStatus();
      buffer_manager->read(ref.offset, ref.length,
                           [&output, &read_done, &read_status](ExternalBufferStatus status,
                                                               Buffer::InstancePtr data) {
                             read_done = true;
                             if (status != ExternalBufferStatus::Ok || data == nullptr) {
                               read_status =
                                   absl::InternalError("failed to read from BufferManager");
                               return;
                             }
                             output.add(*data);
                           });
      if (!read_status.ok()) {
        return read_status;
      }
    }
    output.add("\"");
    return absl::OkStatus();
  }

  switch (node.type()) {
  case nlohmann::json::value_t::null:
    output.add("null");
    break;
  case nlohmann::json::value_t::boolean:
    output.add(node.get<bool>() ? "true" : "false");
    break;
  case nlohmann::json::value_t::number_integer:
  case nlohmann::json::value_t::number_unsigned:
  case nlohmann::json::value_t::number_float:
  case nlohmann::json::value_t::string:
    output.add(node.dump());
    break;
  case nlohmann::json::value_t::array: {
    output.add("[");
    bool first = true;
    for (const auto& item : node) {
      if (!first) {
        output.add(",");
      }
      first = false;
      absl::Status status = serializeNodeSync(item, buffer_manager, output);
      if (!status.ok()) {
        return status;
      }
    }
    output.add("]");
    break;
  }
  case nlohmann::json::value_t::object: {
    output.add("{");
    bool first = true;
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (!first) {
        output.add(",");
      }
      first = false;
      output.add(nlohmann::json(it.key()).dump());
      output.add(":");
      absl::Status status = serializeNodeSync(it.value(), buffer_manager, output);
      if (!status.ok()) {
        return status;
      }
    }
    output.add("}");
    break;
  }
  case nlohmann::json::value_t::binary:
    output.add(node.dump());
    break;
  case nlohmann::json::value_t::discarded:
    return absl::InvalidArgumentError("cannot serialize discarded JSON node");
  }

  return absl::OkStatus();
}

} // namespace

absl::StatusOr<Buffer::OwnedImpl> Serializer::serialize(const JsonWithExtBuf& doc,
                                                        BufferManager* buffer_manager) {
  Buffer::OwnedImpl output;
  absl::Status status = serializeNodeSync(doc.json(), buffer_manager, output);
  if (!status.ok()) {
    return status;
  }
  return output;
}

namespace {

std::string escapeJsonPointer(absl::string_view token) {
  std::string result;
  result.reserve(token.size());
  for (char c : token) {
    if (c == '~') {
      result += "~0";
    } else if (c == '/') {
      result += "~1";
    } else {
      result += c;
    }
  }
  return result;
}

} // namespace

Coroutine::Task<absl::Status> Serializer::serialize(const JsonWithExtBuf& doc,
                                                    BufferManager* buffer_manager,
                                                    ChunkOutput output,
                                                    SinkProvider* sink_provider) {
  Buffer::OwnedImpl small_buf;
  CO_RETURN_IF_ERROR(
      co_await serializeNode(doc.json(), "", buffer_manager, output, small_buf, sink_provider));
  if (small_buf.length() > 0) {
    output(small_buf, true);
  } else {
    Buffer::OwnedImpl empty;
    output(empty, true);
  }
  co_return absl::OkStatus();
}

Coroutine::Task<absl::Status>
Serializer::serializeNode(const nlohmann::json& node, absl::string_view current_path,
                          BufferManager* buffer_manager, ChunkOutput& output,
                          Buffer::OwnedImpl& small_buf, SinkProvider* sink_provider) {
  auto flush_small_buf = [&output, &small_buf]() {
    if (small_buf.length() >= 4096) {
      output(small_buf, false);
      small_buf.drain(small_buf.length());
    }
  };

  auto emit_small_buf = [&output, &small_buf]() {
    if (small_buf.length() > 0) {
      output(small_buf, false);
      small_buf.drain(small_buf.length());
    }
  };

  if (sink_provider != nullptr && !current_path.empty() &&
      sink_provider->hasFieldStream(current_path)) {
    auto buf_or = co_await sink_provider->getFieldStream(current_path);
    if (!buf_or.ok()) {
      co_return buf_or.status();
    }
    if (buf_or->has_value()) {
      auto stream = std::move(buf_or->value());
      small_buf.add("\"");
      emit_small_buf();
      while (true) {
        auto chunk_or = co_await stream.recv();
        if (!chunk_or.ok()) {
          co_return chunk_or.status();
        }
        if (!chunk_or->has_value()) {
          break;
        }
        auto& chunk = chunk_or->value();
        if (chunk.length() > 0) {
          output(chunk, false);
        }
      }
      small_buf.add("\"");
    } else {
      small_buf.add("\"\"");
    }
    flush_small_buf();
    co_return absl::OkStatus();
  }

  if (JsonWithExtBuf::isExternalRef(node)) {
    auto ref_or = JsonWithExtBuf::externalRef(node);
    if (!ref_or.ok()) {
      co_return ref_or.status();
    }
    const auto& ref = *ref_or;
    small_buf.add("\"");
    if (ref.length > 0) {
      if (buffer_manager == nullptr) {
        co_return absl::InternalError("buffer_manager is null for ExternalRef node");
      }
      emit_small_buf();
      uint64_t remaining = ref.length;
      uint64_t current_offset = ref.offset;
      constexpr uint64_t ChunkSize = 64 * 1024;
      while (remaining > 0) {
        uint64_t to_read = std::min(remaining, ChunkSize);
        bool read_done = false;
        absl::Status read_status = absl::OkStatus();
        buffer_manager->read(current_offset, to_read,
                             [&output, &read_done, &read_status](ExternalBufferStatus status,
                                                                 Buffer::InstancePtr data) {
                               read_done = true;
                               if (status != ExternalBufferStatus::Ok || data == nullptr) {
                                 read_status =
                                     absl::InternalError("failed to read from BufferManager");
                                 return;
                               }
                               if (data->length() > 0) {
                                 output(*data, false);
                               }
                             });
        if (!read_status.ok()) {
          co_return read_status;
        }
        current_offset += to_read;
        remaining -= to_read;
      }
    }
    small_buf.add("\"");
    flush_small_buf();
    co_return absl::OkStatus();
  }

  switch (node.type()) {
  case nlohmann::json::value_t::null:
    small_buf.add("null");
    flush_small_buf();
    break;
  case nlohmann::json::value_t::boolean:
    small_buf.add(node.get<bool>() ? "true" : "false");
    flush_small_buf();
    break;
  case nlohmann::json::value_t::number_integer:
  case nlohmann::json::value_t::number_unsigned:
  case nlohmann::json::value_t::number_float:
  case nlohmann::json::value_t::string:
    small_buf.add(node.dump());
    flush_small_buf();
    break;
  case nlohmann::json::value_t::array: {
    small_buf.add("[");
    bool first = true;
    size_t idx = 0;
    for (const auto& item : node) {
      std::string elem_path = absl::StrCat(current_path, "/", idx++);
      if (!first) {
        small_buf.add(",");
      }
      first = false;
      CO_RETURN_IF_ERROR(co_await serializeNode(item, elem_path, buffer_manager, output, small_buf,
                                                sink_provider));
    }
    small_buf.add("]");
    flush_small_buf();
    break;
  }
  case nlohmann::json::value_t::object: {
    small_buf.add("{");
    bool first = true;
    for (auto it = node.begin(); it != node.end(); ++it) {
      std::string child_path = absl::StrCat(current_path, "/", escapeJsonPointer(it.key()));
      if (sink_provider != nullptr && sink_provider->hasFieldStream(child_path)) {
        auto buf_or = co_await sink_provider->getFieldStream(child_path);
        if (!buf_or.ok()) {
          co_return buf_or.status();
        }
        if (!buf_or->has_value()) {
          // Dropped property, omit from output object
          continue;
        }
        if (!first) {
          small_buf.add(",");
        }
        first = false;
        small_buf.add(nlohmann::json(it.key()).dump());
        small_buf.add(":");
        emit_small_buf();

        auto stream = std::move(buf_or->value());
        small_buf.add("\"");
        emit_small_buf();
        while (true) {
          auto chunk_or = co_await stream.recv();
          if (!chunk_or.ok()) {
            co_return chunk_or.status();
          }
          if (!chunk_or->has_value()) {
            break;
          }
          auto& chunk = chunk_or->value();
          if (chunk.length() > 0) {
            output(chunk, false);
          }
        }
        small_buf.add("\"");
        flush_small_buf();
        continue;
      }

      if (!first) {
        small_buf.add(",");
      }
      first = false;
      small_buf.add(nlohmann::json(it.key()).dump());
      small_buf.add(":");
      CO_RETURN_IF_ERROR(co_await serializeNode(it.value(), child_path, buffer_manager, output,
                                                small_buf, sink_provider));
    }
    small_buf.add("}");
    flush_small_buf();
    break;
  }
  case nlohmann::json::value_t::binary:
    small_buf.add(node.dump());
    flush_small_buf();
    break;
  case nlohmann::json::value_t::discarded:
    co_return absl::InvalidArgumentError("cannot serialize discarded JSON node");
  }

  co_return absl::OkStatus();
}

} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
