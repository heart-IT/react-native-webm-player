#import "V2SpikeEngine.h"
#include "spike_fixture_data.inc"

#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <AudioToolbox/AudioToolbox.h>
#include <vector>
#include <memory>
#include <atomic>
#include <opus.h>
#include "mkvparser/mkvparser.h"
#include "video/VP9HeaderParser.h"

#pragma mark - Logging

#define SPIKE_LOG(fmt, ...) NSLog(@"[V2Spike] " fmt, ##__VA_ARGS__)

#pragma mark - Memory-backed IMkvReader

namespace {

class MemoryReader : public mkvparser::IMkvReader {
public:
    MemoryReader(const uint8_t* data, size_t length) : data_(data), length_(length) {}

    int Read(long long pos, long len, unsigned char* buf) override {
        if (len <= 0 || pos < 0) return -1;
        if (static_cast<size_t>(pos) + static_cast<size_t>(len) > length_) return -1;
        std::memcpy(buf, data_ + pos, static_cast<size_t>(len));
        return 0;
    }

    int Length(long long* total, long long* available) override {
        if (total) *total = static_cast<long long>(length_);
        if (available) *available = static_cast<long long>(length_);
        return 0;
    }

private:
    const uint8_t* data_;
    size_t length_;
};

struct AudioPacket {
    std::vector<uint8_t> data;
    int64_t ptsUs;
};

struct VideoPacket {
    std::vector<uint8_t> data;
    int64_t ptsUs;
    bool isKeyframe;
};

struct OpusConfig {
    int sampleRate = 48000;
    int channels = 2;
    std::vector<uint8_t> codecPrivate;
};

struct VP9Config {
    int width = 0;
    int height = 0;
    int profile = 0;
};

}  // namespace

#pragma mark - Demux helpers

static bool DemuxAll(const uint8_t* data, size_t length,
                     OpusConfig& outOpus, VP9Config& outVP9,
                     std::vector<AudioPacket>& outAudio,
                     std::vector<VideoPacket>& outVideo,
                     int64_t& outDurationUs,
                     NSError** error) {
    MemoryReader reader(data, length);

    long long pos = 0;
    auto ebml = std::make_unique<mkvparser::EBMLHeader>();
    long long ebmlStatus = ebml->Parse(&reader, pos);
    if (ebmlStatus < 0) {
        if (error) *error = [NSError errorWithDomain:@"V2Spike" code:1 userInfo:@{NSLocalizedDescriptionKey:@"EBML parse failed"}];
        return false;
    }

    mkvparser::Segment* segmentRaw = nullptr;
    long long segStatus = mkvparser::Segment::CreateInstance(&reader, pos, segmentRaw);
    if (segStatus != 0 || !segmentRaw) {
        if (error) *error = [NSError errorWithDomain:@"V2Spike" code:2 userInfo:@{NSLocalizedDescriptionKey:@"Segment::CreateInstance failed"}];
        return false;
    }
    std::unique_ptr<mkvparser::Segment> segment(segmentRaw);

    long loadStatus = segment->Load();
    if (loadStatus < 0) {
        if (error) *error = [NSError errorWithDomain:@"V2Spike" code:3 userInfo:@{NSLocalizedDescriptionKey:@"Segment::Load failed"}];
        return false;
    }

    const mkvparser::Tracks* tracks = segment->GetTracks();
    if (!tracks) {
        if (error) *error = [NSError errorWithDomain:@"V2Spike" code:4 userInfo:@{NSLocalizedDescriptionKey:@"No tracks"}];
        return false;
    }

    long audioTrackNum = -1;
    long videoTrackNum = -1;
    for (unsigned long i = 0; i < tracks->GetTracksCount(); ++i) {
        const mkvparser::Track* track = tracks->GetTrackByIndex(i);
        if (!track) continue;
        if (track->GetType() == mkvparser::Track::kAudio) {
            const auto* at = static_cast<const mkvparser::AudioTrack*>(track);
            audioTrackNum = at->GetNumber();
            outOpus.sampleRate = static_cast<int>(at->GetSamplingRate());
            outOpus.channels = static_cast<int>(at->GetChannels());
            size_t cpSize = 0;
            const uint8_t* cp = at->GetCodecPrivate(cpSize);
            if (cp && cpSize > 0) outOpus.codecPrivate.assign(cp, cp + cpSize);
            SPIKE_LOG(@"Audio track %ld: %dHz %dch codecPrivate=%zub",
                      audioTrackNum, outOpus.sampleRate, outOpus.channels, cpSize);
        } else if (track->GetType() == mkvparser::Track::kVideo) {
            const auto* vt = static_cast<const mkvparser::VideoTrack*>(track);
            videoTrackNum = vt->GetNumber();
            outVP9.width = static_cast<int>(vt->GetWidth());
            outVP9.height = static_cast<int>(vt->GetHeight());
            SPIKE_LOG(@"Video track %ld: %dx%d", videoTrackNum, outVP9.width, outVP9.height);
        }
    }

    if (audioTrackNum < 0 || videoTrackNum < 0) {
        if (error) *error = [NSError errorWithDomain:@"V2Spike" code:5
            userInfo:@{NSLocalizedDescriptionKey:@"Need both audio and video tracks"}];
        return false;
    }

    int64_t maxPtsUs = 0;
    const mkvparser::Cluster* cluster = segment->GetFirst();
    while (cluster && !cluster->EOS()) {
        const mkvparser::BlockEntry* be = nullptr;
        long entryStatus = cluster->GetFirst(be);
        while (entryStatus == 0 && be && !be->EOS()) {
            const mkvparser::Block* block = be->GetBlock();
            if (block) {
                long long ptsNs = block->GetTime(cluster);
                int64_t ptsUs = ptsNs / 1000;
                long trackNum = static_cast<long>(block->GetTrackNumber());
                int frameCount = block->GetFrameCount();
                for (int f = 0; f < frameCount; ++f) {
                    const mkvparser::Block::Frame& frame = block->GetFrame(f);
                    std::vector<uint8_t> bytes(static_cast<size_t>(frame.len));
                    int readStatus = frame.Read(&reader, bytes.data());
                    if (readStatus != 0) continue;
                    if (trackNum == audioTrackNum) {
                        outAudio.push_back({std::move(bytes), ptsUs});
                    } else if (trackNum == videoTrackNum) {
                        VideoPacket vp;
                        if (block->IsKey() && outVP9.profile == 0 && bytes.size() >= 4) {
                            auto info = media::vp9::parseHeader(bytes.data(), bytes.size());
                            if (info.valid) {
                                outVP9.profile = info.profile;
                                if (info.width > 0) outVP9.width = info.width;
                                if (info.height > 0) outVP9.height = info.height;
                            }
                        }
                        vp.data = std::move(bytes);
                        vp.ptsUs = ptsUs;
                        vp.isKeyframe = block->IsKey();
                        outVideo.push_back(std::move(vp));
                    }
                    if (ptsUs > maxPtsUs) maxPtsUs = ptsUs;
                }
            }
            entryStatus = cluster->GetNext(be, be);
        }
        cluster = segment->GetNext(cluster);
    }

    outDurationUs = maxPtsUs;
    SPIKE_LOG(@"Demuxed: %zu audio packets, %zu video packets, duration=%.2fs",
              outAudio.size(), outVideo.size(), maxPtsUs / 1e6);
    return true;
}

#pragma mark - Audio: Opus → CMSampleBuffer (LPCM float)

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

    CMAudioFormatDescriptionRef formatDesc = nullptr;
    OSStatus status = CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &asbd,
                                                     0, nullptr, 0, nullptr, nullptr, &formatDesc);
    if (status != noErr || !formatDesc) {
        SPIKE_LOG(@"CMAudioFormatDescriptionCreate failed: %d", (int)status);
        return nullptr;
    }

    size_t dataSize = sizeof(float) * static_cast<size_t>(frameCount) * static_cast<size_t>(channels);
    CMBlockBufferRef blockBuffer = nullptr;
    status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, dataSize,
                                                kCFAllocatorDefault, nullptr, 0, dataSize,
                                                kCMBlockBufferAssureMemoryNowFlag, &blockBuffer);
    if (status != noErr || !blockBuffer) {
        CFRelease(formatDesc);
        return nullptr;
    }
    CMBlockBufferReplaceDataBytes(pcm, blockBuffer, 0, dataSize);

    CMSampleTimingInfo timing;
    timing.duration = CMTimeMake(frameCount, sampleRate);
    timing.presentationTimeStamp = CMTimeMake(ptsUs, 1000000);
    timing.decodeTimeStamp = kCMTimeInvalid;

    CMSampleBufferRef sampleBuffer = nullptr;
    status = CMSampleBufferCreate(kCFAllocatorDefault, blockBuffer, true, nullptr, nullptr,
                                  formatDesc, frameCount, 1, &timing, 0, nullptr, &sampleBuffer);
    CFRelease(blockBuffer);
    CFRelease(formatDesc);
    if (status != noErr) {
        SPIKE_LOG(@"CMSampleBufferCreate (audio) failed: %d", (int)status);
        return nullptr;
    }
    return sampleBuffer;
}

#pragma mark - VP9 decompression session (fallback path)

static NSData* BuildVpcCConfig(int profile) {
    uint8_t buf[12] = {0};
    buf[0] = 0x01;
    buf[4] = static_cast<uint8_t>(profile);
    buf[5] = 30;                            // level 3.0
    buf[6] = (8u << 4) | (1u << 1) | 0u;    // 8-bit, 4:2:0, limited range
    buf[7] = 1; buf[8] = 1; buf[9] = 1;     // BT.709
    return [NSData dataWithBytes:buf length:sizeof(buf)];
}

@interface V2SpikeVideoDecoder : NSObject
- (instancetype)initWithLayer:(AVSampleBufferDisplayLayer*)layer
                        width:(int)width
                       height:(int)height
                      profile:(int)profile;
- (BOOL)submitFrame:(const uint8_t*)data length:(size_t)length ptsUs:(int64_t)ptsUs;
- (void)shutdown;
@end

@implementation V2SpikeVideoDecoder {
    __weak AVSampleBufferDisplayLayer* _layer;
    VTDecompressionSessionRef _session;
    CMVideoFormatDescriptionRef _formatDesc;
    int _width;
    int _height;
    int _profile;
    dispatch_queue_t _renderQueue;
}

- (instancetype)initWithLayer:(AVSampleBufferDisplayLayer*)layer
                        width:(int)width height:(int)height profile:(int)profile {
    self = [super init];
    if (!self) return nil;
    _layer = layer;
    _width = width;
    _height = height;
    _profile = profile;
    _renderQueue = dispatch_queue_create("v2spike.video.render", DISPATCH_QUEUE_SERIAL);

    constexpr CMVideoCodecType kVP9CodecType = 'vp09';
    if (@available(iOS 26.2, *)) {
        VTRegisterSupplementalVideoDecoderIfAvailable(kVP9CodecType);
    }
    if (!VTIsHardwareDecodeSupported(kVP9CodecType)) {
        SPIKE_LOG(@"VP9 HW decode not supported on this device");
        return nil;
    }

    NSData* vpcc = BuildVpcCConfig(profile);
    NSDictionary* extensions = @{
        (NSString*)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms: @{ @"vpcC": vpcc }
    };
    OSStatus status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kVP9CodecType,
                                                     width, height,
                                                     (__bridge CFDictionaryRef)extensions,
                                                     &_formatDesc);
    if (status != noErr || !_formatDesc) {
        SPIKE_LOG(@"CMVideoFormatDescriptionCreate failed: %d", (int)status);
        return nil;
    }

    NSDictionary* destAttrs = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (NSString*)kCVPixelBufferWidthKey: @(width),
        (NSString*)kCVPixelBufferHeightKey: @(height),
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}
    };
    VTDecompressionOutputCallbackRecord cb = {0};
    cb.decompressionOutputCallback = &OutputCallback;
    cb.decompressionOutputRefCon = (__bridge void*)self;
    status = VTDecompressionSessionCreate(kCFAllocatorDefault, _formatDesc, nullptr,
                                          (__bridge CFDictionaryRef)destAttrs, &cb, &_session);
    if (status != noErr || !_session) {
        SPIKE_LOG(@"VTDecompressionSessionCreate failed: %d", (int)status);
        return nil;
    }
    SPIKE_LOG(@"VP9 session created %dx%d profile=%d", width, height, profile);
    return self;
}

- (void)shutdown {
    if (_session) {
        VTDecompressionSessionWaitForAsynchronousFrames(_session);
        VTDecompressionSessionInvalidate(_session);
        CFRelease(_session);
        _session = nullptr;
    }
    if (_formatDesc) {
        CFRelease(_formatDesc);
        _formatDesc = nullptr;
    }
}

- (void)dealloc { [self shutdown]; }

- (BOOL)submitFrame:(const uint8_t*)data length:(size_t)length ptsUs:(int64_t)ptsUs {
    if (!_session || !data || length == 0) return NO;

    CMBlockBufferRef block = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, length,
                                                         kCFAllocatorDefault, nullptr, 0, length,
                                                         kCMBlockBufferAssureMemoryNowFlag, &block);
    if (status != noErr || !block) return NO;
    CMBlockBufferReplaceDataBytes(data, block, 0, length);

    size_t sampleSize = length;
    CMSampleTimingInfo timing;
    timing.duration = CMTimeMake(1, 30);
    timing.presentationTimeStamp = CMTimeMake(ptsUs, 1000000);
    timing.decodeTimeStamp = kCMTimeInvalid;

    CMSampleBufferRef sample = nullptr;
    status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, _formatDesc, 1, 1, &timing,
                                       1, &sampleSize, &sample);
    CFRelease(block);
    if (status != noErr || !sample) return NO;

    void* ptsCtx = reinterpret_cast<void*>(static_cast<uintptr_t>(ptsUs));
    VTDecodeFrameFlags flags = kVTDecodeFrame_EnableAsynchronousDecompression;
    VTDecodeInfoFlags out = 0;
    status = VTDecompressionSessionDecodeFrame(_session, sample, flags, ptsCtx, &out);
    CFRelease(sample);
    if (status != noErr) {
        SPIKE_LOG(@"DecodeFrame failed: %d (pts=%lldus)", (int)status, ptsUs);
        return NO;
    }
    return YES;
}

static void OutputCallback(void* refCon, void* sourceFrameRefCon, OSStatus status,
                            VTDecodeInfoFlags flags, CVImageBufferRef imageBuffer,
                            CMTime, CMTime) {
    if (status != noErr || !imageBuffer || (flags & kVTDecodeInfo_FrameDropped)) return;
    auto* self_ = (__bridge V2SpikeVideoDecoder*)refCon;
    int64_t ptsUs = static_cast<int64_t>(reinterpret_cast<uintptr_t>(sourceFrameRefCon));

    CMVideoFormatDescriptionRef outFmt = nullptr;
    OSStatus s = CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, imageBuffer, &outFmt);
    if (s != noErr) return;

    CMSampleTimingInfo timing;
    timing.duration = CMTimeMake(1, 30);
    timing.presentationTimeStamp = CMTimeMake(ptsUs, 1000000);
    timing.decodeTimeStamp = kCMTimeInvalid;

    CMSampleBufferRef sample = nullptr;
    s = CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, imageBuffer, outFmt, &timing, &sample);
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

@end

#pragma mark - Engine

@implementation V2SpikeEngine {
    AVSampleBufferDisplayLayer* _displayLayer;
    AVSampleBufferAudioRenderer* _audioRenderer;
    AVSampleBufferRenderSynchronizer* _synchronizer;
    V2SpikeVideoDecoder* _videoDecoder;
    OpusDecoder* _opusDecoder;
    OpusConfig _opusConfig;
    Float64 _durationSeconds;
    NSUInteger _audioCount;
    NSUInteger _videoCount;
    std::atomic<BOOL> _stopped;
    id _endObserver;
}

- (instancetype)initWithDisplayLayer:(AVSampleBufferDisplayLayer*)displayLayer
                       audioRenderer:(AVSampleBufferAudioRenderer*)audioRenderer
                        synchronizer:(AVSampleBufferRenderSynchronizer*)synchronizer {
    self = [super init];
    if (!self) return nil;
    _displayLayer = displayLayer;
    _audioRenderer = audioRenderer;
    _synchronizer = synchronizer;
    _stopped = NO;
    return self;
}

- (void)dealloc {
    [self stop];
    if (_opusDecoder) opus_decoder_destroy(_opusDecoder);
}

- (NSUInteger)audioPacketsEnqueued { return _audioCount; }
- (NSUInteger)videoPacketsEnqueued { return _videoCount; }
- (Float64)fixtureDurationSeconds { return _durationSeconds; }

- (void)playFixtureWithCompletion:(V2SpikeEngineCompletion)completion {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError* err = nil;
        OpusConfig opusCfg;
        VP9Config vp9Cfg;
        std::vector<AudioPacket> audioPackets;
        std::vector<VideoPacket> videoPackets;
        int64_t durationUs = 0;
        if (!DemuxAll(spike_fixture_webm, spike_fixture_webm_len,
                      opusCfg, vp9Cfg, audioPackets, videoPackets, durationUs, &err)) {
            dispatch_async(dispatch_get_main_queue(), ^{ completion(err); });
            return;
        }

        self->_opusConfig = opusCfg;
        self->_durationSeconds = durationUs / 1e6;

        int opusErr = 0;
        self->_opusDecoder = opus_decoder_create(opusCfg.sampleRate, opusCfg.channels, &opusErr);
        if (!self->_opusDecoder || opusErr != OPUS_OK) {
            NSError* e = [NSError errorWithDomain:@"V2Spike" code:10
                userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"opus_decoder_create=%d", opusErr]}];
            dispatch_async(dispatch_get_main_queue(), ^{ completion(e); });
            return;
        }

        dispatch_sync(dispatch_get_main_queue(), ^{
            self->_videoDecoder = [[V2SpikeVideoDecoder alloc] initWithLayer:self->_displayLayer
                                                                       width:vp9Cfg.width
                                                                      height:vp9Cfg.height
                                                                     profile:vp9Cfg.profile];
        });
        if (!self->_videoDecoder) {
            NSError* e = [NSError errorWithDomain:@"V2Spike" code:11
                userInfo:@{NSLocalizedDescriptionKey:@"VP9 decoder init failed"}];
            dispatch_async(dispatch_get_main_queue(), ^{ completion(e); });
            return;
        }

        // Decode + enqueue all audio packets
        constexpr int kMaxFrameSize = 5760;  // 120ms @ 48kHz
        std::vector<float> pcm(kMaxFrameSize * static_cast<size_t>(opusCfg.channels));
        for (const auto& pkt : audioPackets) {
            if (self->_stopped) break;
            int frames = opus_decode_float(self->_opusDecoder, pkt.data.data(),
                                           static_cast<int>(pkt.data.size()),
                                           pcm.data(), kMaxFrameSize, 0);
            if (frames <= 0) {
                SPIKE_LOG(@"opus_decode_float returned %d", frames);
                continue;
            }
            CMSampleBufferRef sb = BuildLPCMSampleBuffer(pcm.data(), frames,
                                                         opusCfg.sampleRate, opusCfg.channels,
                                                         pkt.ptsUs);
            if (!sb) continue;
            [self->_audioRenderer enqueueSampleBuffer:sb];
            CFRelease(sb);
            self->_audioCount++;
        }

        // Submit all video packets — VTDecompression will fire callbacks that enqueue into layer
        for (const auto& pkt : videoPackets) {
            if (self->_stopped) break;
            [self->_videoDecoder submitFrame:pkt.data.data()
                                       length:pkt.data.size()
                                        ptsUs:pkt.ptsUs];
            self->_videoCount++;
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            if (self->_stopped) { completion(nil); return; }

            CMTime endTime = CMTimeMake(durationUs, 1000000);
            __weak V2SpikeEngine* weakSelf = self;
            self->_endObserver = [self->_synchronizer addBoundaryTimeObserverForTimes:@[[NSValue valueWithCMTime:endTime]]
                                                                                 queue:dispatch_get_main_queue()
                                                                            usingBlock:^{
                V2SpikeEngine* strongSelf = weakSelf;
                if (!strongSelf) return;
                [strongSelf->_synchronizer setRate:0];
                if (strongSelf->_endObserver) {
                    [strongSelf->_synchronizer removeTimeObserver:strongSelf->_endObserver];
                    strongSelf->_endObserver = nil;
                }
                completion(nil);
            }];

            [self->_synchronizer setRate:1.0];
            SPIKE_LOG(@"Playback started, audio=%lu video=%lu duration=%.2fs",
                      (unsigned long)self->_audioCount, (unsigned long)self->_videoCount,
                      self->_durationSeconds);
        });
    });
}

- (void)stop {
    _stopped = YES;
    if (_synchronizer) [_synchronizer setRate:0];
    if (_endObserver) {
        [_synchronizer removeTimeObserver:_endObserver];
        _endObserver = nil;
    }
    if (_videoDecoder) {
        [_videoDecoder shutdown];
        _videoDecoder = nil;
    }
}

@end
