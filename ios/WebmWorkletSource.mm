#import "WebmWorkletSource.h"

#import <BareKit/BareKit.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "common/MediaLog.h"

namespace {

// framed-stream's default framing: a 4-byte little-endian length per frame.
constexpr size_t kFrameHeaderBytes = 4;
constexpr uint8_t kFrameData = 0;
constexpr uint8_t kFrameStatus = 1;
// The worklet flushes avio every packet from a 4 KiB buffer; a frame anywhere
// near this did not come from it.
constexpr uint32_t kMaxFrameBytes = 16u * 1024 * 1024;
// How long the reader waits for `readable` before re-checking the stop flag.
constexpr int64_t kReadableWaitMs = 100;
// Retry interval when the sink has no room.
constexpr int kSinkRetryMs = 5;

// bare-kit requires the entry filename to end in .bundle.
NSString* const kWorkletFilename = @"/feeder.bundle";

NSData* LoadFeederBundle() {
  // A static pod's resource bundle lands in the app's main bundle; a framework
  // pod carries it inside its own bundle.
  NSBundle* pod = [NSBundle bundleForClass:[WebmWorkletSource class]];
  for (NSBundle* host in @[ pod, NSBundle.mainBundle ]) {
    NSURL* url = [host URLForResource:@"WebmPlayerWorklet" withExtension:@"bundle"];
    if (!url) continue;
    NSString* path = [[NSBundle bundleWithURL:url] pathForResource:@"feeder"
                                                             ofType:@"bundle"];
    if (path) return [NSData dataWithContentsOfFile:path];
  }
  return nil;
}

}  // namespace

@implementation WebmWorkletSource {
  BareWorklet* _worklet;
  BareIPC* _ipc;
  // BareWorklet does not retain the source bytes; they must outlive it.
  NSData* _bundle;
  NSString* _storageDir;
  WebmByteSink _sink;
  dispatch_semaphore_t _readable;
  std::thread _reader;
  std::atomic<bool> _stopping;
  std::atomic<uint64_t> _bytesReceived;
  bool _loggedUnknownFrame;  // reader thread only
}

- (instancetype)init {
  self = [super init];
  if (!self) return nil;
  _readable = dispatch_semaphore_create(0);
  _stopping = false;
  _bytesReceived = 0;
  _loggedUnknownFrame = false;
  return self;
}

- (void)dealloc {
  [self stop];
}

- (uint64_t)bytesReceived {
  return _bytesReceived.load();
}

- (BOOL)startWithKey:(NSString*)key sink:(WebmByteSink)sink {
  if (_worklet) return YES;
  _bundle = LoadFeederBundle();
  if (!_bundle) {
    MEDIA_LOG_E("WebmWorkletSource: WebmPlayerWorklet/feeder.bundle resource missing");
    return NO;
  }
  _sink = [sink copy];
  _storageDir = [NSTemporaryDirectory()
      stringByAppendingPathComponent:[NSString stringWithFormat:@"webm-player/%@",
                                                                NSUUID.UUID.UUIDString]];
  [NSFileManager.defaultManager createDirectoryAtPath:_storageDir
                          withIntermediateDirectories:YES
                                           attributes:nil
                                                error:nil];
  _stopping = false;
  _worklet = [[BareWorklet alloc] initWithConfiguration:nil];
  [_worklet start:kWorkletFilename source:_bundle arguments:@[ key, _storageDir ]];
  _ipc = [[BareIPC alloc] initWithWorklet:_worklet];
  // Runs on bare-kit's own thread: only wake the reader from here.
  dispatch_semaphore_t readable = _readable;
  _ipc.readable = ^(BareIPC*) {
    dispatch_semaphore_signal(readable);
  };
  _reader = std::thread([self] { [self run]; });
  return YES;
}

- (void)stop {
  if (!_worklet) return;
  // The reader is joined before anything it reads from is torn down; its
  // wait is bounded by kReadableWaitMs and the sink retry checks the flag.
  _stopping = true;
  if (_reader.joinable()) _reader.join();
  [_worklet terminate];
  [_ipc close];
  _ipc = nil;
  _worklet = nil;
  _bundle = nil;
  _sink = nil;
  [NSFileManager.defaultManager removeItemAtPath:_storageDir error:nil];
}

#pragma mark - Reader thread

- (void)run {
  pthread_setname_np("webmplayer.worklet");
  std::vector<uint8_t> buffer;
  bool ended = false;
  while (!_stopping.load()) {
    @autoreleasepool {
      // nil: nothing ready yet. Empty: the worklet closed its end.
      NSData* data = [_ipc read];
      if (!data) {
        dispatch_semaphore_wait(
            _readable, dispatch_time(DISPATCH_TIME_NOW, kReadableWaitMs * NSEC_PER_MSEC));
        continue;
      }
      if (data.length == 0) {
        ended = true;
        break;
      }
      auto* bytes = static_cast<const uint8_t*>(data.bytes);
      buffer.insert(buffer.end(), bytes, bytes + data.length);
      if (![self drainFrames:buffer]) {
        ended = true;
        break;
      }
    }
  }
  if (!ended || _stopping.load()) return;
  // Weak-load the delegate on main rather than here: taking the last strong
  // reference on this thread would run dealloc → stop → join on itself.
  __weak WebmWorkletSource* weakSelf = self;
  dispatch_async(dispatch_get_main_queue(), ^{
    WebmWorkletSource* self_ = weakSelf;
    if (!self_ || self_->_stopping.load()) return;
    [self_.delegate workletSourceDidEnd:self_];
  });
}

/// Consumes every complete frame in `buffer`. Returns NO on a frame the
/// protocol cannot have produced, after reporting it as an error status.
- (BOOL)drainFrames:(std::vector<uint8_t>&)buffer {
  size_t offset = 0;
  while (buffer.size() - offset >= kFrameHeaderBytes) {
    const uint8_t* p = buffer.data() + offset;
    uint32_t length = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                      (static_cast<uint32_t>(p[2]) << 16) |
                      (static_cast<uint32_t>(p[3]) << 24);
    if (length == 0 || length > kMaxFrameBytes) {
      MEDIA_LOG_E("WebmWorkletSource: bad frame length %u", length);
      [self postStatus:@"error protocol: bad frame length"];
      return NO;
    }
    if (buffer.size() - offset < kFrameHeaderBytes + length) break;
    const uint8_t* payload = p + kFrameHeaderBytes;
    [self handleFrameType:payload[0] body:payload + 1 length:length - 1];
    offset += kFrameHeaderBytes + length;
  }
  buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
  return YES;
}

- (void)handleFrameType:(uint8_t)type body:(const uint8_t*)body length:(size_t)length {
  switch (type) {
    case kFrameData:
      [self deliverData:body length:length];
      break;
    case kFrameStatus:
      [self postStatus:[[NSString alloc] initWithBytes:body
                                                length:length
                                              encoding:NSUTF8StringEncoding]
                           ?: @"error protocol: status not UTF-8"];
      break;
    default:
      if (!_loggedUnknownFrame) {
        _loggedUnknownFrame = true;
        MEDIA_LOG_W("WebmWorkletSource: ignoring frame type %u", type);
      }
  }
}

- (void)deliverData:(const uint8_t*)bytes length:(size_t)length {
  size_t offset = 0;
  while (offset < length && !_stopping.load()) {
    size_t wrote = _sink(bytes + offset, length - offset);
    if (wrote == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kSinkRetryMs));
      continue;
    }
    offset += wrote;
    _bytesReceived.fetch_add(wrote);
  }
}

- (void)postStatus:(NSString*)status {
  __weak WebmWorkletSource* weakSelf = self;
  dispatch_async(dispatch_get_main_queue(), ^{
    WebmWorkletSource* self_ = weakSelf;
    if (!self_ || self_->_stopping.load()) return;
    [self_.delegate workletSource:self_ didReportStatus:status];
  });
}

@end
