#include <csignal>
#include <iostream>
#include <string>

#include "contrib/ext_proc_cache/cache_age_calculator.h"
#include "contrib/ext_proc_cache/cache_key_generator.h"
#include "contrib/ext_proc_cache/cache_lookup_coordinator.h"
#include "contrib/ext_proc_cache/cacheability_checker.h"
#include "contrib/ext_proc_cache/in_memory_cache_store.h"
#include "contrib/ext_proc_cache/server.h"

std::function<void(int)> shutdown_handler;

void signal_handler(int signal) {
  if (shutdown_handler) {
    shutdown_handler(signal);
  }
}

int main(int argc, char** argv) {
  std::string address = "0.0.0.0:50051";
  if (argc > 1) {
    address = argv[1];
  }

  // Wire default implementations.
  auto store = std::make_shared<Envoy::Extensions::ExtProcCache::InMemoryCacheStore>();
  auto key_gen = std::make_shared<Envoy::Extensions::ExtProcCache::DefaultCacheKeyGenerator>();
  auto cacheability = std::make_shared<Envoy::Extensions::ExtProcCache::DefaultCacheabilityChecker>();
  auto age_calc = std::make_shared<Envoy::Extensions::ExtProcCache::DefaultCacheAgeCalculator>();
  auto coordinator = std::make_shared<Envoy::Extensions::ExtProcCache::CacheLookupCoordinator>(store);

  Envoy::Extensions::ExtProcCache::ExtProcCacheServer server;
  server.start(address, coordinator, key_gen, cacheability, age_calc);

  std::cout << "ExtProcCache server listening on " << address
            << " (port " << server.port() << ")" << std::endl;

  shutdown_handler = [&server](int) {
    std::cout << "Shutting down..." << std::endl;
    server.shutdown();
  };

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  // Block until Shutdown() is called by the signal handler.
  server.wait();

  return 0;
}
