// Simplified: epoch callbacks removed (PTS discontinuity handled inline in AudioDecodeSlot)
#pragma once

#include <array>
#include <mutex>
#include <functional>
#include "common/MediaConfig.h"
#include "common/MediaLog.h"
#include "common/CompilerHints.h"
#include "Participant.h"
#include "AudioDecodeSlot.h"
#include "SyncController.h"
#include "DecodeThread.h"

namespace media {

class DecodeThread;

// Eviction reason for JS notification
enum class EvictionReason : uint8_t {
    LRUSlotExhaustion = 0,  // All slots full, evicted least-recently-used
    InactivityTimeout = 1    // Participant exceeded inactivity threshold
};

// Callback invoked when a participant is evicted from their audio slot.
// Parameters: evictedParticipantId, newParticipantId (-1 if inactivity), reason
using EvictionCallback = std::function<void(int evictedId, int replacedById, EvictionReason reason)>;

// Pending eviction info for deferred callback invocation outside mutex
struct PendingEviction {
    int evictedId = -1;
    int replacedById = -1;
    EvictionReason reason = EvictionReason::LRUSlotExhaustion;
    EvictionCallback cb;

    explicit operator bool() const noexcept { return evictedId >= 0 && cb; }
    void invoke() noexcept { if (*this) cb(evictedId, replacedById, reason); }
};

// Thread safety: Mutex-protected slot allocation with lock-free fast path.
// - admitParticipant() has lock-free fast path for already-admitted participants
// - Slow path (allocation/eviction) protected by mtx_
// - Called from JS thread only (frame push path)
class SlotAdmission {
public:
    static constexpr int kMaxAudioSlots = config::limits::kMaxAudioSlots;

    SlotAdmission(ParticipantRegistry& participants,
                  std::array<AudioDecodeSlot*, kMaxAudioSlots>& audioSlots,
                  SyncController& sync,
                  DecodeThread& decodeThread) noexcept
        : participants_(participants)
        , audioSlots_(audioSlots)
        , sync_(sync)
        , decodeThread_(decodeThread) {}

    SlotAdmission(const SlotAdmission&) = delete;
    SlotAdmission& operator=(const SlotAdmission&) = delete;

    // Admits a participant to an audio slot, allocating or reusing via LRU eviction.
    // Returns true if participant now has a slot (either already had one, or newly assigned).
    // Callers should NOT pre-check hasAudioSlot() - this function handles that atomically
    // under lock to avoid TOCTOU races.
    [[nodiscard]] bool admitParticipant(int participantId) noexcept {
        // Fast-path: Check if participant already has a slot (lock-free).
        // This is the common case during active streaming - avoids mutex
        // contention on every frame push (50 calls/sec per participant).
        Participant* p = participants_.get(participantId);
        if (!p) return false;

        if (LIKELY(p->hasAudioSlot())) {
            return true;
        }

        // Slow-path: Needs slot allocation - take mutex.
        // Eviction callback deferred outside lock to prevent deadlock.
        PendingEviction pending;
        bool result = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);

            // Re-check under lock (another thread may have allocated)
            if (p->hasAudioSlot()) return true;

            int slot = findFreeAudioSlot();
            if (slot >= 0) {
                bindAudioSlot(participantId, slot);
                p->setState(ParticipantState::Active);
                return true;
            }

            slot = evictLRUSlot(participantId, pending);
            if (slot >= 0) {
                bindAudioSlot(participantId, slot);
                p->setState(ParticipantState::Active);
                result = true;
            }
        }

        pending.invoke();
        return result;
    }

    void releaseParticipant(int participantId) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);

        Participant* p = participants_.get(participantId);
        if (!p) return;

        releaseAudioSlot(participantId);
        p->setState(ParticipantState::Disconnected);
    }

    [[nodiscard]] int audioSlotFor(int participantId) const noexcept {
        const Participant* p = participants_.get(participantId);
        return p ? p->audioSlot() : -1;
    }

    [[nodiscard]] size_t activeSlotCount() const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        size_t count = 0;
        for (size_t i = 0; i < static_cast<size_t>(kMaxAudioSlots); ++i) {
            if (audioSlots_[i] && audioSlots_[i]->isBound()) ++count;
        }
        return count;
    }

    void setEvictionCallback(EvictionCallback cb) noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        evictionCallback_ = std::move(cb);
    }

    void releaseInactiveSlot(int slotIndex) noexcept {
        if (slotIndex < 0 || slotIndex >= kMaxAudioSlots) return;

        EvictionCallback cb;
        int boundPid = -1;
        {
            std::lock_guard<std::mutex> lk(mtx_);

            AudioDecodeSlot* slot = audioSlots_[static_cast<size_t>(slotIndex)];
            if (!slot || !slot->isBound()) return;

            boundPid = slot->boundParticipant();
            if (boundPid < 0) return;

            Participant* p = participants_.get(boundPid);
            if (!p) return;

            int64_t inactiveUs = nowUs() - p->lastActivityUs();
            if (inactiveUs < config::admission::kInactivityTimeoutUs) return;

            MEDIA_LOG_I("SlotAdmission: releasing inactive participant %d (idle %llds)",
                        boundPid, static_cast<long long>(inactiveUs / 1'000'000));

            releaseAudioSlotInternal(boundPid, slotIndex);
            cb = evictionCallback_;
        }

        if (cb) {
            cb(boundPid, config::sentinel::kNoParticipant, EvictionReason::InactivityTimeout);
        }
    }

private:
    [[nodiscard]] int findFreeAudioSlot() const noexcept {
        for (size_t i = 0; i < static_cast<size_t>(kMaxAudioSlots); ++i) {
            if (audioSlots_[i] && !audioSlots_[i]->isBound()) return static_cast<int>(i);
        }
        return -1;
    }

    // Populates pending eviction for deferred callback invocation (caller invokes outside lock)
    [[nodiscard]] int evictLRUSlot(int requestingParticipantId, PendingEviction& pending) noexcept {
        int64_t now = nowUs();
        int lruSlot = -1;
        int64_t oldestActivity = INT64_MAX;
        int lruParticipantId = -1;

        for (size_t i = 0; i < static_cast<size_t>(kMaxAudioSlots); ++i) {
            if (!audioSlots_[i] || !audioSlots_[i]->isBound()) continue;

            int boundPid = audioSlots_[i]->boundParticipant();
            if (boundPid < 0) continue;

            Participant* p = participants_.get(boundPid);
            if (!p) continue;

            int64_t lastActivity = p->lastActivityUs();
            int64_t inactiveUs = now - lastActivity;

            if (inactiveUs < config::admission::kMinSlotHoldUs) continue;

            if (lastActivity < oldestActivity) {
                oldestActivity = lastActivity;
                lruSlot = static_cast<int>(i);
                lruParticipantId = boundPid;
            }
        }

        if (lruSlot >= 0 && lruParticipantId >= 0) {
            int64_t inactiveMs = (now - oldestActivity) / 1000;
            MEDIA_LOG_I("SlotAdmission: evicting participant %d (inactive %lldms) for participant %d",
                        lruParticipantId, static_cast<long long>(inactiveMs), requestingParticipantId);

            releaseAudioSlotInternal(lruParticipantId, lruSlot);

            pending.evictedId = lruParticipantId;
            pending.replacedById = requestingParticipantId;
            pending.reason = EvictionReason::LRUSlotExhaustion;
            pending.cb = evictionCallback_;
        }

        return lruSlot;
    }

    void bindAudioSlot(int participantId, int slotIndex) noexcept {
        if (slotIndex < 0 || slotIndex >= kMaxAudioSlots) return;

        Participant* p = participants_.get(participantId);
        AudioDecodeSlot* slot = audioSlots_[static_cast<size_t>(slotIndex)];

        if (!p || !slot) return;

        slot->bind(participantId);
        p->setAudioSlot(slotIndex);
        p->updateActivity();

        decodeThread_.notifySlotBound(slotIndex);

        sync_.getJitter(participantId);
    }

    void releaseAudioSlot(int participantId) noexcept {
        Participant* p = participants_.get(participantId);
        if (!p) return;

        int slot = p->audioSlot();
        if (slot >= 0 && slot < kMaxAudioSlots) {
            releaseAudioSlotInternal(participantId, slot);
        }
    }

    void releaseAudioSlotInternal(int participantId, int slotIndex) noexcept {
        if (slotIndex < 0 || slotIndex >= kMaxAudioSlots) return;

        // unbind() before notifySlotUnbound(): unbind() sets encodedClearRequested_
        // which the decode thread services via serviceDeferredClear(). The decode
        // thread must still visit this slot (via activeSlotMask_) to process the
        // deferred clear before we remove it from the mask. Visiting an unbound
        // slot is safe — processPendingDecode() checks isBound() and exits early.
        if (audioSlots_[static_cast<size_t>(slotIndex)]) {
            audioSlots_[static_cast<size_t>(slotIndex)]->unbind();
        }

        decodeThread_.notifySlotUnbound(slotIndex);

        Participant* p = participants_.get(participantId);
        if (p) {
            p->setAudioSlot(config::sentinel::kNoSlot);
            p->setState(ParticipantState::Connected);
        }

        // NOTE: Do NOT release jitter estimator here - preserves buffer depth
        // across LRU eviction/re-admission cycles. Jitter state is only released
        // when participant is explicitly removed via releaseParticipant().
    }

    mutable std::mutex mtx_;
    ParticipantRegistry& participants_;
    std::array<AudioDecodeSlot*, kMaxAudioSlots>& audioSlots_;
    SyncController& sync_;
    DecodeThread& decodeThread_;
    EvictionCallback evictionCallback_;
};

}  // namespace media
