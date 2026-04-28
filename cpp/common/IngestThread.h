// Dedicated thread for WebM demuxing and packet dispatch.
//
// Consumes raw WebM bytes from IngestRingBuffer (written by JS thread),
// runs the pipeline graph (demux + dispatch + clip metadata), all off the JS thread.
//
// Delegates lifecycle to WorkerThread<T>.
#pragma once

#include <atomic>
#include <functional>
#include "IngestRingBuffer.h"
#include "MediaConfig.h"
#include "MediaLog.h"
#include "MediaTime.h"
#include "WorkerThread.h"
#include "pipeline/BroadcastPipeline.h"

namespace media {

// Processor: demuxes available bytes from the ring buffer each cycle.
class IngestProcessor {
public:
    using RetainPosUpdater = std::function<void()>;

    struct Deps {
        IngestRingBuffer& ringBuffer;
        pipeline::BroadcastPipeline& pipeline;
        RetainPosUpdater retainPosUpdater;
    };

    explicit IngestProcessor(Deps deps) noexcept
        : deps_(std::move(deps)) {
        lastProcessedPos_.store(deps_.ringBuffer.currentWritePos(),
                                std::memory_order_relaxed);
    }

    [[nodiscard]] bool process() noexcept {
        size_t wp = deps_.ringBuffer.currentWritePos();
        size_t last = lastProcessedPos_.load(std::memory_order_relaxed);
        if (wp <= last) return false;

        size_t newBytes = wp - last;
        auto result = deps_.pipeline.onNewData(newBytes);

        if (result.recoveryNeeded) {
            recoveryNeeded_.store(true, std::memory_order_release);
        }

        lastProcessedPos_.store(wp, std::memory_order_relaxed);

        // Advance decode retention every cycle to prevent ring compaction
        // from overwriting data still needed by audio/video decode queues
        if (deps_.retainPosUpdater) deps_.retainPosUpdater();

        return true;
    }

    void onDetached() noexcept {}

    // syncPosition and process() must happen-before each other; caller must
    // guarantee the thread is paused before syncPosition() is called.
    void syncPosition() noexcept {
        lastProcessedPos_.store(deps_.ringBuffer.currentWritePos(),
                                std::memory_order_relaxed);
    }

    [[nodiscard]] bool consumeRecoveryNeeded() noexcept {
        return recoveryNeeded_.exchange(false, std::memory_order_acq_rel);
    }

private:
    Deps deps_;
    std::atomic<bool> recoveryNeeded_{false};
    std::atomic<size_t> lastProcessedPos_{0};
};

// Facade: preserves the public API used by PipelineOrchestrator.
class IngestThread {
public:
    struct Deps {
        IngestRingBuffer& ringBuffer;
        pipeline::BroadcastPipeline& pipeline;
        IngestProcessor::RetainPosUpdater retainPosUpdater;
    };

    explicit IngestThread(Deps deps) noexcept
        : processor_({deps.ringBuffer, deps.pipeline, std::move(deps.retainPosUpdater)})
        , worker_(processor_, {"WebMIngest",
                               config::thread::kDecodeLoopSleepUs,
                               5, false, true}) {}

    ~IngestThread() noexcept = default;

    IngestThread(const IngestThread&) = delete;
    IngestThread& operator=(const IngestThread&) = delete;

    [[nodiscard]] bool start() noexcept { return worker_.start(); }
    void stop() noexcept { worker_.stop(); }
    void wake() noexcept { worker_.wake(); }
    // Returns true if the thread acknowledged the pause within the deadline.
    // Callers that mutate shared state (ring buffer reset, position sync) MUST
    // check this and abort on false instead of proceeding with racy mutations.
    [[nodiscard]] bool pause() noexcept { return worker_.pause(); }
    void resume() noexcept { worker_.resume(); }

    [[nodiscard]] bool isRunning() const noexcept { return worker_.isRunning(); }
    [[nodiscard]] bool wasDetached() const noexcept { return worker_.wasDetached(); }

    [[nodiscard]] bool isResponsive() const noexcept {
        // IngestThread uses a const-compatible check (no watchdog trip tracking)
        int64_t heartbeat = worker_.timeSinceLastHeartbeatUs();
        if (heartbeat == 0) return true;
        return heartbeat < config::thread::kHealthLogIntervalUs * 3;
    }

    [[nodiscard]] int64_t timeSinceLastHeartbeatUs() const noexcept {
        return worker_.timeSinceLastHeartbeatUs();
    }

    void syncPosition() noexcept { processor_.syncPosition(); }

    [[nodiscard]] bool consumeRecoveryNeeded() noexcept {
        return processor_.consumeRecoveryNeeded();
    }

private:
    IngestProcessor processor_;
    WorkerThread<IngestProcessor> worker_;
};

}  // namespace media
