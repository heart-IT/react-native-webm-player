// Background thread for speech transcription.
// Reads PCM from TranscriptRingBuffer, detects voice activity,
// runs whisper.cpp inference, and pushes results to TranscriptRegistry.
//
// VAD-gated chunking: accumulates audio until silence is detected (>300ms),
// then triggers inference on the accumulated speech. Falls back to 2s max
// chunk if no silence is detected (caps latency).
//
// Threading: owns its std::thread. All public methods called from JS thread.
#pragma once

#include <thread>
#include <atomic>
#include <memory>
#include <cmath>
#include "common/MediaLog.h"
#include "common/MediaTime.h"
#include "common/ThreadAffinity.h"
#include "common/TimedJoin.h"
#include "common/MediaConfig.h"
#include "transcript/TranscriptRingBuffer.h"
#include "transcript/TranscriptEngine.h"
#include "transcript/TranscriptRegistry.h"

namespace media::transcript {

namespace config {
    // Ring buffer: next power of 2 above 5s at 48kHz (240000 → 262144)
    constexpr size_t kRingBufferSamples = 262144;

    // VAD parameters
    constexpr float kSilenceThreshold = 0.005f;     // RMS below this = silence
    constexpr int kSilenceFrames = 15;               // 15 * 20ms = 300ms of silence to trigger
    constexpr size_t kMinChunkSamples = kSourceSampleRate * 1;  // Min 1s before processing
    constexpr size_t kMaxChunkSamples48k = kSourceSampleRate * 2;  // Max 2s before forced processing

    // Read chunk size: 20ms at 48kHz = 960 samples (one audio frame)
    constexpr size_t kReadChunkSamples = 960;
}

class TranscriptThread {
public:
    TranscriptThread() = default;
    ~TranscriptThread() { stop(); }

    TranscriptThread(const TranscriptThread&) = delete;
    TranscriptThread& operator=(const TranscriptThread&) = delete;

    // Create ring buffer, init engine, start thread.
    // modelPath: path to .bin whisper model file.
    bool start(const std::string& modelPath) {
        if (running_.load(std::memory_order_acquire)) return true;

        ringBuffer_ = std::make_unique<TranscriptRingBuffer>(config::kRingBufferSamples);

        engine_ = std::make_unique<TranscriptEngine>();
        if (!engine_->init(modelPath)) {
            MEDIA_LOG_E("TranscriptThread: engine init failed");
            engine_.reset();
            ringBuffer_.reset();
            return false;
        }

        threadExited_.store(false, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&TranscriptThread::run, this);
        return true;
    }

    void stop() {
        if (!running_.load(std::memory_order_acquire)) return;
        running_.store(false, std::memory_order_release);
        timedJoin(thread_, threadExited_,
                  ::media::config::thread::kThreadJoinTimeoutMs, "TranscriptThread");
        engine_.reset();
        ringBuffer_.reset();
    }

    // Returns raw pointer for atomic storage in MediaSession.
    // Lifetime managed by this thread — valid between start() and stop().
    TranscriptRingBuffer* ringBuffer() noexcept { return ringBuffer_.get(); }

    [[nodiscard]] bool isRunning() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    void setTranslateToEnglish(bool translate) noexcept {
        if (engine_) engine_->setTranslateToEnglish(translate);
    }

private:
    void run() {
        thread_affinity::setThreadName("Transcript");
        // Whisper spawns 4 OpenMP threads during inference. Lower priority so they
        // don't contend with audio/video decode on mid-tier devices.
        (void)thread_affinity::configureCurrentThreadForBackground();

        float readBuf[config::kReadChunkSamples];
        std::vector<float> accumBuf;
        accumBuf.reserve(config::kMaxChunkSamples48k);

        int silenceCount = 0;
        int64_t chunkStartUs = nowUs();

        MEDIA_LOG_I("TranscriptThread: started");

        while (running_.load(std::memory_order_acquire)) {
            bool overflowed = false;
            size_t read = ringBuffer_->read(readBuf, config::kReadChunkSamples, &overflowed);
            if (read == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // On ring buffer overflow, discard accumulated state — audio was skipped
            if (overflowed) {
                accumBuf.clear();
                silenceCount = 0;
                chunkStartUs = nowUs();
                continue;
            }

            // Cap insert to prevent accumBuf from exceeding max chunk
            size_t room = config::kMaxChunkSamples48k > accumBuf.size()
                        ? config::kMaxChunkSamples48k - accumBuf.size()
                        : 0;
            size_t toAppend = std::min(read, room);
            if (toAppend > 0) {
                accumBuf.insert(accumBuf.end(), readBuf, readBuf + toAppend);
            }

            // VAD: compute RMS energy of this chunk
            float energy = computeRMS(readBuf, read);
            if (energy < config::kSilenceThreshold) {
                silenceCount++;
            } else {
                silenceCount = 0;
            }

            // Decide whether to trigger inference
            bool silenceGate = silenceCount >= config::kSilenceFrames &&
                               accumBuf.size() >= config::kMinChunkSamples;
            bool maxLengthGate = accumBuf.size() >= config::kMaxChunkSamples48k;

            if (silenceGate || maxLengthGate) {
                if (accumBuf.size() >= config::kMinChunkSamples) {
                    auto segments = engine_->process(
                        accumBuf.data(), accumBuf.size(), chunkStartUs);

                    for (auto& seg : segments) {
                        TranscriptRegistry::instance().pushSegment(seg);
                    }
                }

                accumBuf.clear();
                silenceCount = 0;
                chunkStartUs = nowUs();
            }
        }

        threadExited_.store(true, std::memory_order_release);
        MEDIA_LOG_I("TranscriptThread: stopped");
    }

    static float computeRMS(const float* data, size_t count) noexcept {
        if (count == 0) return 0.0f;
        float sum = 0.0f;
        for (size_t i = 0; i < count; ++i) {
            sum += data[i] * data[i];
        }
        return std::sqrt(sum / static_cast<float>(count));
    }

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> threadExited_{false};

    std::unique_ptr<TranscriptRingBuffer> ringBuffer_;
    std::unique_ptr<TranscriptEngine> engine_;
};

}  // namespace media::transcript
