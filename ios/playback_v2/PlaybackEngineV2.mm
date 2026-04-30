#import "PlaybackEngineV2.h"

#import <VideoToolbox/VideoToolbox.h>
#import <AudioToolbox/AudioToolbox.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <opus.h>
#include "mkvparser/mkvparser.h"
#include "common/WebMStreamBuffer.h"
#include "common/MediaLog.h"
#include "video/VP9HeaderParser.h"

namespace {

#pragma mark - IMkvReader over WebMStreamBuffer

// Read-window IMkvReader: presents a sliding window of bytes pulled from the SPSC
// ring on demand. mkvparser indexes globally (long long pos), but we only retain
// the recent N MB. compact() drops bytes the parser has finished consuming.
class RingReader final : public mkvparser::IMkvReader {
public:
    explicit RingReader(media::WebMStreamBuffer& ring) : ring_(ring) {}

    // Drain ring into our window. Call from demux thread on each pass.
    size_t pumpFromRing(uint64_t timeoutMs) {
        constexpr size_t kChunk = 64 * 1024;
        if (window_.size() - windowEnd_ < kChunk) {
            window_.resize(windowEnd_ + kChunk);
        }
        int got = ring_.read(window_.data() + windowEnd_, kChunk, timeoutMs);
        if (got <= 0) return 0;
        windowEnd_ += static_cast<size_t>(got);
        return static_cast<size_t>(got);
    }

    int Read(long long pos, long len, unsigned char* buf) override {
        if (len <= 0 || pos < 0) return -1;
        long long offsetInWindow = pos - baseOffset_;
        if (offsetInWindow < 0) return -1;
        size_t off = static_cast<size_t>(offsetInWindow);
        size_t need = static_cast<size_t>(len);
        if (off + need > windowEnd_) return -1;
        std::memcpy(buf, window_.data() + off, need);
        return 0;
    }

    int Length(long long* total, long long* available) override {
        if (total) *total = -1;
        if (available) *available = baseOffset_ + static_cast<long long>(windowEnd_);
        return 0;
    }

    void compactBefore(long long pos) {
        long long offsetInWindow = pos - baseOffset_;
        if (offsetInWindow <= 0) return;
        size_t drop = static_cast<size_t>(offsetInWindow);
        if (drop > windowEnd_) drop = windowEnd_;
        if (drop < (1 << 20)) return;  // only compact when worthwhile (>1MB)
        window_.erase(window_.begin(), window_.begin() + static_cast<ptrdiff_t>(drop));
        windowEnd_ -= drop;
        baseOffset_ += static_cast<long long>(drop);
    }

private:
    media::WebMStreamBuffer& ring_;
    std::vector<uint8_t> window_;
    size_t windowEnd_ = 0;
    long long baseOffset_ = 0;
};

#pragma mark - Helpers

static NSData* BuildVpcCConfig(int profile) {
    uint8_t buf[12] = {0};
    buf[0] = 0x01;
    buf[4] = static_cast<uint8_t>(profile);
    buf[5] = 30;
    buf[6] = (8u << 4) | (1u << 1) | 0u;
    buf[7] = 1; buf[8] = 1; buf[9] = 1;
    return [NSData dataWithBytes:buf length:sizeof(buf)];
}

static CMSampleBufferRef BuildLPCMSampleBuffer(const float* pcm, int frameCount,
                                                int sampleRate, int channels,
                                                int64_t ptsUs) {
    AudioStreamBasicDescription asbd = {0};
    asbd.mSampleRate = sampleRate;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mBytesPerPacket = static_cast<UInt32>(sizeof(float) * channels);
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = static_cast<UInt32>(sizeof(float) * channels);
    asbd.mChannelsPerFrame = static_cast<UInt32>(channels);
    asbd.mBitsPerChannel = 32;

    CMAudioFormatDescriptionRef fmt = nullptr;
    if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &asbd, 0, nullptr, 0, nullptr, nullptr, &fmt) != noErr) return nullptr;

    size_t dataSize = sizeof(float) * static_cast<size_t>(frameCount) * static_cast<size_t>(channels);
    CMBlockBufferRef block = nullptr;
    if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, dataSize,
                                            kCFAllocatorDefault, nullptr, 0, dataSize,
                                            kCMBlockBufferAssureMemoryNowFlag, &block) != noErr) {
        CFRelease(fmt); return nullptr;
    }
    CMBlockBufferReplaceDataBytes(pcm, block, 0, dataSize);

    CMSampleTimingInfo timing;
    timing.duration = CMTimeMake(frameCount, sampleRate);
    timing.presentationTimeStamp = CMTimeMake(ptsUs, 1000000);
    timing.decodeTimeStamp = kCMTimeInvalid;

    CMSampleBufferRef sb = nullptr;
    OSStatus s = CMSampleBufferCreate(kCFAllocatorDefault, block, true, nullptr, nullptr,
                                      fmt, frameCount, 1, &timing, 0, nullptr, &sb);
    CFRelease(block); CFRelease(fmt);
    if (s != noErr) return nullptr;
    return sb;
}

}  // namespace

#pragma mark - VP9 decoder shim

@interface PlaybackEngineV2VideoDecoder : NSObject
- (void)setOutputLayer:(AVSampleBufferDisplayLayer*)layer;
- (BOOL)submitFrame:(const uint8_t*)data length:(size_t)length ptsUs:(int64_t)ptsUs isKey:(BOOL)isKey;
- (void)shutdown;
@end

@implementation PlaybackEngineV2VideoDecoder {
    __weak AVSampleBufferDisplayLayer* _layer;
    VTDecompressionSessionRef _session;
    CMVideoFormatDescriptionRef _formatDesc;
    int _width;
    int _height;
    int _profile;
    BOOL _hwSupported;
    dispatch_queue_t _renderQueue;
}

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _renderQueue = dispatch_queue_create("playbackv2.video.render", DISPATCH_QUEUE_SERIAL);
    constexpr CMVideoCodecType kVP9CodecType = 'vp09';
    if (@available(iOS 26.2, *)) {
        VTRegisterSupplementalVideoDecoderIfAvailable(kVP9CodecType);
    }
    _hwSupported = VTIsHardwareDecodeSupported(kVP9CodecType);
    return self;
}

- (void)dealloc { [self shutdown]; }

- (void)setOutputLayer:(AVSampleBufferDisplayLayer*)layer { _layer = layer; }

- (BOOL)ensureSession:(int)width height:(int)height profile:(int)profile {
    if (_session && width == _width && height == _height && profile == _profile) return YES;
    [self shutdownSession];
    if (!_hwSupported) return NO;

    NSData* vpcc = BuildVpcCConfig(profile);
    NSDictionary* extensions = @{
        (NSString*)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms: @{ @"vpcC": vpcc }
    };
    constexpr CMVideoCodecType kVP9CodecType = 'vp09';
    OSStatus s = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kVP9CodecType,
                                                width, height,
                                                (__bridge CFDictionaryRef)extensions, &_formatDesc);
    if (s != noErr || !_formatDesc) return NO;

    NSDictionary* destAttrs = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (NSString*)kCVPixelBufferWidthKey: @(width),
        (NSString*)kCVPixelBufferHeightKey: @(height),
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}
    };
    VTDecompressionOutputCallbackRecord cb = {0};
    cb.decompressionOutputCallback = &OutputCallback;
    cb.decompressionOutputRefCon = (__bridge void*)self;
    s = VTDecompressionSessionCreate(kCFAllocatorDefault, _formatDesc, nullptr,
                                     (__bridge CFDictionaryRef)destAttrs, &cb, &_session);
    if (s != noErr || !_session) {
        CFRelease(_formatDesc); _formatDesc = nullptr;
        return NO;
    }
    _width = width; _height = height; _profile = profile;
    return YES;
}

- (BOOL)submitFrame:(const uint8_t*)data length:(size_t)length ptsUs:(int64_t)ptsUs isKey:(BOOL)isKey {
    if (!data || length == 0) return NO;

    if (isKey) {
        auto info = media::vp9::parseHeader(data, length);
        if (info.valid && info.width > 0 && info.height > 0) {
            if (![self ensureSession:info.width height:info.height profile:info.profile]) return NO;
        }
    }
    if (!_session) return NO;

    CMBlockBufferRef block = nullptr;
    OSStatus s = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, length,
                                                    kCFAllocatorDefault, nullptr, 0, length,
                                                    kCMBlockBufferAssureMemoryNowFlag, &block);
    if (s != noErr || !block) return NO;
    CMBlockBufferReplaceDataBytes(data, block, 0, length);

    size_t sampleSize = length;
    CMSampleTimingInfo timing;
    timing.duration = CMTimeMake(1, 30);
    timing.presentationTimeStamp = CMTimeMake(ptsUs, 1000000);
    timing.decodeTimeStamp = kCMTimeInvalid;

    CMSampleBufferRef sample = nullptr;
    s = CMSampleBufferCreateReady(kCFAllocatorDefault, block, _formatDesc, 1, 1, &timing,
                                  1, &sampleSize, &sample);
    CFRelease(block);
    if (s != noErr || !sample) return NO;

    void* ptsCtx = reinterpret_cast<void*>(static_cast<uintptr_t>(ptsUs));
    VTDecodeInfoFlags out = 0;
    s = VTDecompressionSessionDecodeFrame(_session, sample, kVTDecodeFrame_EnableAsynchronousDecompression,
                                          ptsCtx, &out);
    CFRelease(sample);
    return s == noErr;
}

static void OutputCallback(void* refCon, void* sourceFrameRefCon, OSStatus status,
                            VTDecodeInfoFlags flags, CVImageBufferRef imageBuffer,
                            CMTime, CMTime) {
    if (status != noErr || !imageBuffer || (flags & kVTDecodeInfo_FrameDropped)) return;
    auto* self_ = (__bridge PlaybackEngineV2VideoDecoder*)refCon;
    int64_t ptsUs = static_cast<int64_t>(reinterpret_cast<uintptr_t>(sourceFrameRefCon));

    CMVideoFormatDescriptionRef outFmt = nullptr;
    if (CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, imageBuffer, &outFmt) != noErr) return;

    CMSampleTimingInfo timing;
    timing.duration = CMTimeMake(1, 30);
    timing.presentationTimeStamp = CMTimeMake(ptsUs, 1000000);
    timing.decodeTimeStamp = kCMTimeInvalid;

    CMSampleBufferRef sample = nullptr;
    OSStatus s = CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, imageBuffer, outFmt, &timing, &sample);
    CFRelease(outFmt);
    if (s != noErr || !sample) return;

    AVSampleBufferDisplayLayer* layer = self_->_layer;
    CFRetain(sample);
    dispatch_async(self_->_renderQueue, ^{
        if (layer) [layer enqueueSampleBuffer:sample];
        CFRelease(sample);
    });
    CFRelease(sample);
}

- (void)shutdownSession {
    if (_session) {
        VTDecompressionSessionWaitForAsynchronousFrames(_session);
        VTDecompressionSessionInvalidate(_session);
        CFRelease(_session); _session = nullptr;
    }
    if (_formatDesc) { CFRelease(_formatDesc); _formatDesc = nullptr; }
    _width = _height = _profile = 0;
}

- (void)shutdown { [self shutdownSession]; }

@end

#pragma mark - Engine

@implementation PlaybackEngineV2 {
    std::unique_ptr<media::WebMStreamBuffer> _ring;
    AVSampleBufferDisplayLayer* _displayLayer;
    AVSampleBufferAudioRenderer* _audioRenderer;
    AVSampleBufferRenderSynchronizer* _synchronizer;
    PlaybackEngineV2VideoDecoder* _videoDecoder;
    OpusDecoder* _opusDecoder;
    int _opusSampleRate;
    int _opusChannels;
    long _audioTrackNum;
    long _videoTrackNum;
    BOOL _tracksParsed;

    std::thread _demuxThread;
    std::atomic<bool> _running;
    std::atomic<bool> _paused;
    std::atomic<bool> _stop;

    std::atomic<uint64_t> _bytesFed;
    std::atomic<uint64_t> _audioPacketsDecoded;
    std::atomic<uint64_t> _videoPacketsDecoded;
    std::atomic<float> _gain;
    std::atomic<bool> _muted;
    std::atomic<float> _playbackRate;

    void (^_healthCallback)(NSString*, NSString*);
}

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _running = NO; _paused = NO; _stop = NO;
    _bytesFed = 0; _audioPacketsDecoded = 0; _videoPacketsDecoded = 0;
    _gain = 1.0f; _muted = NO; _playbackRate = 1.0f;
    return self;
}

- (void)dealloc { [self stop]; }

- (void)setDisplayLayer:(AVSampleBufferDisplayLayer*)layer {
    _displayLayer = layer;
    if (_videoDecoder) [_videoDecoder setOutputLayer:layer];
    if (_synchronizer && layer) {
        // Attach if not already a renderer
        @try { [_synchronizer addRenderer:layer]; } @catch (...) {}
    }
}

- (BOOL)start {
    if (_running) return YES;
    _ring = std::make_unique<media::WebMStreamBuffer>(media::WebMStreamBuffer::getDefaultCapacity());

    _audioRenderer = [[AVSampleBufferAudioRenderer alloc] init];
    _audioRenderer.muted = _muted.load();
    _audioRenderer.volume = _gain.load();
    _synchronizer = [[AVSampleBufferRenderSynchronizer alloc] init];
    [_synchronizer addRenderer:_audioRenderer];
    if (_displayLayer) [_synchronizer addRenderer:_displayLayer];

    _videoDecoder = [[PlaybackEngineV2VideoDecoder alloc] init];
    [_videoDecoder setOutputLayer:_displayLayer];

    [self setupAudioSession];

    _stop = NO; _paused = NO; _running = YES;
    _demuxThread = std::thread([self]{ [self demuxLoop]; });

    [_synchronizer setRate:_playbackRate.load()];
    return YES;
}

- (BOOL)stop {
    if (!_running) return YES;
    _stop = YES; _running = NO;
    if (_ring) _ring->shutdown();
    if (_demuxThread.joinable()) _demuxThread.join();

    if (_synchronizer) {
        [_synchronizer setRate:0];
        if (_audioRenderer) [_synchronizer removeRenderer:_audioRenderer atTime:kCMTimeInvalid completionHandler:nil];
        if (_displayLayer) [_synchronizer removeRenderer:_displayLayer atTime:kCMTimeInvalid completionHandler:nil];
    }
    [_videoDecoder shutdown]; _videoDecoder = nil;
    if (_opusDecoder) { opus_decoder_destroy(_opusDecoder); _opusDecoder = nullptr; }
    _audioRenderer = nil; _synchronizer = nil;
    _ring.reset();
    return YES;
}

- (BOOL)pause {
    _paused = YES;
    if (_synchronizer) [_synchronizer setRate:0];
    return YES;
}

- (BOOL)resume {
    _paused = NO;
    if (_synchronizer) [_synchronizer setRate:_playbackRate.load()];
    return YES;
}

- (BOOL)isRunning { return _running.load(); }
- (BOOL)isPaused { return _paused.load(); }

- (size_t)feedData:(const uint8_t*)bytes length:(size_t)length {
    if (!_ring || !_running) return 0;
    size_t wrote = _ring->write(bytes, length, /*isClusterBoundary*/false);
    _bytesFed.fetch_add(wrote);
    return wrote;
}

- (BOOL)setEndOfStream { if (_ring) _ring->setEndOfStream(true); return YES; }

- (BOOL)resetStream {
    if (_ring) _ring->clear();
    _tracksParsed = NO;
    return YES;
}

- (BOOL)setMuted:(BOOL)muted { _muted = muted; if (_audioRenderer) _audioRenderer.muted = muted; return YES; }
- (BOOL)setGain:(float)gain { _gain = gain; if (_audioRenderer) _audioRenderer.volume = gain; return YES; }
- (BOOL)setPlaybackRate:(float)rate {
    _playbackRate = rate;
    if (_synchronizer && _running && !_paused) [_synchronizer setRate:rate];
    return YES;
}

- (PlaybackEngineState)playbackState {
    if (!_running) return PlaybackEngineStateIdle;
    if (_paused) return PlaybackEngineStatePaused;
    if (_videoPacketsDecoded.load() == 0 && _audioPacketsDecoded.load() == 0) return PlaybackEngineStateBuffering;
    return PlaybackEngineStatePlaying;
}

- (PlaybackEngineV2Metrics)metrics {
    PlaybackEngineV2Metrics m = {0};
    m.bytesFedTotal = _bytesFed.load();
    m.audioPacketsDecoded = _audioPacketsDecoded.load();
    m.videoPacketsDecoded = _videoPacketsDecoded.load();
    m.gain = _gain.load();
    m.muted = _muted.load();
    m.playbackRate = _playbackRate.load();
    if (_synchronizer) {
        CMTime t = [_synchronizer currentTime];
        if (CMTIME_IS_VALID(t)) m.currentTimeSeconds = CMTimeGetSeconds(t);
    }
    return m;
}

- (void)setHealthCallback:(void (^)(NSString*, NSString*))callback {
    _healthCallback = [callback copy];
}

- (void)setupAudioSession {
    AVAudioSession* s = [AVAudioSession sharedInstance];
    NSError* err = nil;
    [s setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&err];
    [s setActive:YES error:&err];
}

#pragma mark - Demux loop

- (void)demuxLoop {
    pthread_setname_np("webmplayer.demux");
    if (!_ring) return;
    RingReader reader(*_ring);

    long long pos = 0;
    auto ebml = std::make_unique<mkvparser::EBMLHeader>();
    bool ebmlParsed = false;
    mkvparser::Segment* segmentRaw = nullptr;
    std::unique_ptr<mkvparser::Segment> segment;
    bool tracksLoaded = false;
    const mkvparser::Cluster* cluster = nullptr;
    const mkvparser::BlockEntry* blockEntry = nullptr;

    auto pumpReader = [&](uint64_t timeoutMs) -> bool {
        return reader.pumpFromRing(timeoutMs) > 0;
    };

    constexpr int kMaxOpusFrames = 5760;
    std::vector<float> pcm(static_cast<size_t>(kMaxOpusFrames) * 2);

    while (!_stop.load()) {
        // Pump some data
        if (!pumpReader(50)) {
            if (_ring->isDestroyed()) break;
            continue;
        }

        // Parse EBML header once
        if (!ebmlParsed) {
            long long status = ebml->Parse(&reader, pos);
            if (status < 0) continue;  // need more data
            ebmlParsed = true;
        }

        // Create segment once
        if (!segment) {
            long long s = mkvparser::Segment::CreateInstance(&reader, pos, segmentRaw);
            if (s != 0 || !segmentRaw) continue;
            segment.reset(segmentRaw);
        }

        // Parse tracks once
        if (!tracksLoaded) {
            long s = segment->ParseHeaders();
            if (s < 0) continue;
            const mkvparser::Tracks* tracks = segment->GetTracks();
            if (!tracks) continue;
            for (unsigned long i = 0; i < tracks->GetTracksCount(); ++i) {
                const mkvparser::Track* tr = tracks->GetTrackByIndex(i);
                if (!tr) continue;
                if (tr->GetType() == mkvparser::Track::kAudio) {
                    const auto* at = static_cast<const mkvparser::AudioTrack*>(tr);
                    _audioTrackNum = at->GetNumber();
                    _opusSampleRate = static_cast<int>(at->GetSamplingRate());
                    _opusChannels = static_cast<int>(at->GetChannels());
                    int err = 0;
                    _opusDecoder = opus_decoder_create(_opusSampleRate, _opusChannels, &err);
                } else if (tr->GetType() == mkvparser::Track::kVideo) {
                    _videoTrackNum = static_cast<const mkvparser::VideoTrack*>(tr)->GetNumber();
                }
            }
            tracksLoaded = true;
            _tracksParsed = YES;
        }

        // Drain new clusters (uses LoadCluster pattern from wedge fix)
        for (;;) {
            long s = segment->LoadCluster();
            if (s != 0) break;
        }

        // Iterate clusters → blocks → frames → packets
        if (!cluster || cluster->EOS()) cluster = segment->GetFirst();
        while (cluster && !cluster->EOS()) {
            if (!blockEntry) (void)cluster->GetFirst(blockEntry);
            while (blockEntry && !blockEntry->EOS()) {
                const mkvparser::Block* block = blockEntry->GetBlock();
                if (block) {
                    long long ptsNs = block->GetTime(cluster);
                    int64_t ptsUs = ptsNs / 1000;
                    long trackNum = static_cast<long>(block->GetTrackNumber());
                    int frameCount = block->GetFrameCount();
                    for (int f = 0; f < frameCount; ++f) {
                        const mkvparser::Block::Frame& frame = block->GetFrame(f);
                        std::vector<uint8_t> bytes(static_cast<size_t>(frame.len));
                        if (frame.Read(&reader, bytes.data()) != 0) continue;

                        if (trackNum == _audioTrackNum && _opusDecoder) {
                            int frames = opus_decode_float(_opusDecoder, bytes.data(),
                                                            static_cast<int>(bytes.size()),
                                                            pcm.data(), kMaxOpusFrames, 0);
                            if (frames > 0) {
                                CMSampleBufferRef sb = BuildLPCMSampleBuffer(pcm.data(), frames,
                                                                              _opusSampleRate, _opusChannels, ptsUs);
                                if (sb) {
                                    [_audioRenderer enqueueSampleBuffer:sb];
                                    CFRelease(sb);
                                    _audioPacketsDecoded.fetch_add(1);
                                }
                            }
                        } else if (trackNum == _videoTrackNum) {
                            [_videoDecoder submitFrame:bytes.data() length:bytes.size()
                                                  ptsUs:ptsUs isKey:block->IsKey() ? YES : NO];
                            _videoPacketsDecoded.fetch_add(1);
                        }
                    }
                }
                const mkvparser::BlockEntry* next = nullptr;
                if (cluster->GetNext(blockEntry, next) != 0) break;
                blockEntry = next;
            }
            blockEntry = nullptr;
            cluster = segment->GetNext(cluster);
        }
    }
}

@end
