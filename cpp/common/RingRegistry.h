// Gives an opaque integer handle real ownership semantics.
//
// A raw pointer handed across an FFI boundary cannot express lifetime: the far
// side may issue a call after the near side has freed the object, and there is
// no reliable way to prove it will not. On Android specifically,
// ExoPlayerImplInternal.release() blocks with a timeout and returns false on
// expiry, and Loader.release() shuts its executor down without awaitTermination
// — so a DataSource read can still be in flight, or yet to start, when the
// player is considered released.
//
// The registry closes that: a caller resolves a handle to a shared_ptr for the
// duration of its use, and releasing a handle only drops the registry's own
// reference. A late call finds nothing and fails cleanly instead of touching
// freed memory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "WebMStreamBuffer.h"

namespace media {

class RingRegistry {
 public:
  static RingRegistry& instance();

  RingRegistry(const RingRegistry&) = delete;
  RingRegistry& operator=(const RingRegistry&) = delete;

  /// Creates a ring and returns its handle. Returns 0 on failure.
  int64_t create(size_t capacityBytes, const WebMStreamBuffer::Config& cfg);

  /// Resolves a handle. Returns nullptr once the handle has been released.
  /// The buffer cannot be freed while the returned reference is held.
  std::shared_ptr<WebMStreamBuffer> acquire(int64_t handle) const;

  /// Drops the registry's reference. Callers still holding one from acquire()
  /// keep the buffer alive until they finish.
  void release(int64_t handle);

  /// Live handle count. For tests.
  size_t size() const;

 private:
  RingRegistry() = default;

  mutable std::mutex mutex_;
  int64_t nextHandle_ = 1;
  std::unordered_map<int64_t, std::shared_ptr<WebMStreamBuffer>> rings_;
};

}  // namespace media
