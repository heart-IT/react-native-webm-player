#include "RingRegistry.h"

namespace media {

RingRegistry& RingRegistry::instance() {
  static RingRegistry registry;
  return registry;
}

int64_t RingRegistry::create(size_t capacityBytes,
                             const WebMStreamBuffer::Config& cfg) {
  auto ring = std::make_shared<WebMStreamBuffer>(capacityBytes, cfg);
  std::lock_guard<std::mutex> lock(mutex_);
  int64_t handle = nextHandle_++;
  rings_.emplace(handle, std::move(ring));
  return handle;
}

std::shared_ptr<WebMStreamBuffer> RingRegistry::acquire(int64_t handle) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = rings_.find(handle);
  return it == rings_.end() ? nullptr : it->second;
}

void RingRegistry::release(int64_t handle) {
  // Moved out and dropped after the lock: ~WebMStreamBuffer waits for in-flight
  // consumers, and holding the registry lock across that wait would block every
  // other handle's acquire() — including the consumer we are waiting on.
  std::shared_ptr<WebMStreamBuffer> doomed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = rings_.find(handle);
    if (it == rings_.end()) return;
    doomed = std::move(it->second);
    rings_.erase(it);
  }
}

size_t RingRegistry::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return rings_.size();
}

}  // namespace media
