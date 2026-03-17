#pragma once

#include <coroutine>
#include <memory>

#include "envoy/service/ext_proc/v3/external_processor.grpc.pb.h"
#include "envoy/service/ext_proc/v3/external_processor.pb.h"

#include "grpc++/server.h"

namespace Envoy {
namespace Extensions {
namespace ExtProcCache {

using ProcessingRequest = envoy::service::ext_proc::v3::ProcessingRequest;
using ProcessingResponse = envoy::service::ext_proc::v3::ProcessingResponse;

// A fire-and-forget coroutine task. The coroutine starts eagerly and is not
// awaited — it drives itself via co_await on gRPC reactor operations.
struct Task {
  struct promise_type {
    Task get_return_object() { return {}; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() { std::terminate(); }
  };
};

// A coroutine-driven gRPC ServerBidiReactor for the ExternalProcessor service.
// gRPC callbacks (OnReadDone, OnWriteDone) resume the coroutine, so no threads
// are needed beyond the gRPC completion queue threads.
class ExtProcCacheReactor
    : public grpc::ServerBidiReactor<ProcessingRequest, ProcessingResponse> {
public:
  ExtProcCacheReactor();

  void OnReadDone(bool ok) override;
  void OnWriteDone(bool ok) override;
  void OnDone() override;

private:
  // The main processing coroutine.
  Task run();

  // Build a response for the given request.
  static ProcessingResponse buildResponse(const ProcessingRequest& request);

  // Awaitable that wraps StartRead. Resumes coroutine in OnReadDone.
  struct ReadAwaitable {
    ExtProcCacheReactor* reactor;
    ProcessingRequest* request;
    bool await_ready() const { return false; }
    void await_suspend(std::coroutine_handle<> h);
    bool await_resume() const;
  };

  // Awaitable that wraps StartWrite. Resumes coroutine in OnWriteDone.
  struct WriteAwaitable {
    ExtProcCacheReactor* reactor;
    const ProcessingResponse* response;
    bool await_ready() const { return false; }
    void await_suspend(std::coroutine_handle<> h);
    bool await_resume() const;
  };

  std::coroutine_handle<> read_handle_;
  std::coroutine_handle<> write_handle_;
  bool read_ok_ = false;
  bool write_ok_ = false;
  ProcessingRequest request_;
  ProcessingResponse response_;
};

// Callback-based service that returns a coroutine-driven reactor per stream.
class ExtProcCacheService
    : public envoy::service::ext_proc::v3::ExternalProcessor::CallbackService {
public:
  grpc::ServerBidiReactor<ProcessingRequest, ProcessingResponse>*
  Process(grpc::CallbackServerContext* context) override;
};

// Manages the gRPC server lifecycle.
class ExtProcCacheServer {
public:
  void start(const std::string& address);
  void shutdown();
  void wait();
  int port() const { return listening_port_; }

private:
  std::unique_ptr<ExtProcCacheService> service_;
  std::unique_ptr<grpc::Server> server_;
  int listening_port_ = 0;
};

} // namespace ExtProcCache
} // namespace Extensions
} // namespace Envoy
