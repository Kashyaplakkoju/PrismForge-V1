#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

namespace PrismForge::Video {

// Capture/decode must not wait for effects or output work. Only one frame may be
// executing and one newer frame waiting; Submit replaces the waiting frame.
// Lifecycle Start/destruction belong to an external owner, never the callback.
// Stop from the callback cancels acceptance and defers the join to external Stop.
// The callback borrows an owned AVFrame until it returns. Retain with av_frame_clone,
// and producers must respect FFmpeg copy-on-write before reusing referenced pixels.
class LatestFrameDelivery final {
public:
    using Callback = std::function<void(uint64_t, AVFrame*, double)>;
    struct Stats {
        uint64_t submitted = 0;  // Accepted frames, including replaced frames.
        uint64_t replaced = 0;
        uint64_t delivered = 0;  // Completed callback attempts, including exceptions.
        uint64_t rejected = 0;
        uint64_t callbackErrors = 0;
        uint64_t purged = 0;     // Pending frames discarded on generation/stop.
        unsigned pending = 0;
        unsigned inFlight = 0;
        unsigned maxOutstanding = 0;
    };

    LatestFrameDelivery() = default;
    ~LatestFrameDelivery() { Stop(); }
    LatestFrameDelivery(const LatestFrameDelivery&) = delete;
    LatestFrameDelivery& operator=(const LatestFrameDelivery&) = delete;

    // Restarts are serialized with external Stop. A callback cannot restart or
    // destroy its own worker: the owner must do that after returning from it.
    bool Start(uint64_t generation, Callback callback) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_workerId == std::this_thread::get_id()) return false;
        }
        std::lock_guard<std::mutex> lifecycle(m_lifecycle);
        StopLocked();
        if (!callback) return false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_generation = generation;
            ++m_epoch;
            m_stats = {};
            m_callback = std::move(callback);
            m_stopped = false;
            m_accepting = true;
        }
        try { m_worker = std::thread([this] { Run(); }); }
        catch (...) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_accepting = false;
            m_stopped = true;
            m_callback = {};
            return false;
        }
        return true;
    }

    // The owner's source gate must reject an already executing old callback.
    // Pending/slow cloning old submissions are invalidated even if a token repeats.
    void SetGeneration(uint64_t generation) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_generation = generation;
        ++m_epoch;
        DiscardPending();
    }

    bool Submit(uint64_t generation, const AVFrame* frame) {
        uint64_t epoch = 0, ticket = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_accepting || generation != m_generation) {
                ++m_stats.rejected;
                return false;
            }
            epoch = m_epoch;
            ticket = ++m_nextTicket;
        }
        // Referenced software frames take references, not a full pixel copy.
        // Fully borrowed software frames are copied by av_frame_clone/ref itself.
        Frame owned(IsValidSoftwareFrame(frame) ? av_frame_clone(frame) : nullptr);
        if (!owned) {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_stats.rejected;
            return false;
        }
        const auto now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_accepting || generation != m_generation || epoch != m_epoch ||
                ticket <= m_committedTicket) {
                ++m_stats.rejected;
                return false;
            }
            m_committedTicket = ticket;
            if (m_pending) ++m_stats.replaced;
            m_pending = std::move(owned);
            m_pendingGeneration = generation;
            m_pendingAt = now;
            ++m_stats.submitted;
            m_stats.maxOutstanding = (std::max)(m_stats.maxOutstanding,
                unsigned(bool(m_pending)) + unsigned(m_inFlight));
        }
        m_ready.notify_one();
        return true;
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_workerId == std::this_thread::get_id()) {
                RequestStop();
                m_ready.notify_all();
                return; // External owner joins after this callback returns.
            }
        }
        std::lock_guard<std::mutex> lifecycle(m_lifecycle);
        StopLocked();
    }

    Stats GetStats() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto stats = m_stats;
        stats.pending = unsigned(bool(m_pending));
        stats.inFlight = unsigned(m_inFlight);
        return stats;
    }

private:
    using Clock = std::chrono::steady_clock;
    struct FrameDeleter { void operator()(AVFrame* frame) const { av_frame_free(&frame); } };
    using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
    mutable std::mutex m_mutex;
    std::mutex m_lifecycle;
    std::condition_variable m_ready;
    std::thread m_worker;
    std::thread::id m_workerId;
    Callback m_callback;
    Frame m_pending;
    Clock::time_point m_pendingAt;
    uint64_t m_generation = 0, m_epoch = 0, m_nextTicket = 0, m_committedTicket = 0;
    uint64_t m_pendingGeneration = 0;
    bool m_accepting = false, m_stopped = true, m_inFlight = false;
    Stats m_stats;

    static bool IsValidSoftwareFrame(const AVFrame* frame) {
        if (!frame || frame->width <= 0 || frame->height <= 0 ||
            frame->width > 16384 || frame->height > 16384 ||
            int64_t(frame->width) * frame->height > 64LL * 1024 * 1024) return false;
        const auto format = static_cast<AVPixelFormat>(frame->format);
        const auto* desc = av_pix_fmt_desc_get(format);
        if (!desc || (desc->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BITSTREAM |
            AV_PIX_FMT_FLAG_PAL))) return false;
        int rowBytes[4]{};
        if (av_image_fill_linesizes(rowBytes, format, frame->width) < 0) return false;
        const int planes = av_pix_fmt_count_planes(format);
        if (planes <= 0 || planes > 4) return false;
        for (int p = 0; p < planes; ++p) {
            int rows = frame->height;
            if (!(desc->flags & AV_PIX_FMT_FLAG_RGB) && (p == 1 || p == 2))
                rows = AV_CEIL_RSHIFT(rows, desc->log2_chroma_h);
            const int64_t pitch = frame->linesize[p];
            const uint64_t absPitch = pitch < 0 ? uint64_t(-pitch) : uint64_t(pitch);
            if (!frame->extended_data || frame->extended_data[p] != frame->data[p] ||
                !frame->data[p] || rowBytes[p] <= 0 || absPitch < uint64_t(rowBytes[p])) return false;
            const uint64_t rowOffset = absPitch * uint64_t(rows - 1);
            const uint64_t span = rowOffset + uint64_t(rowBytes[p]);
            if (span > (1ULL << 30)) return false;
            auto* owner = av_frame_get_plane_buffer(const_cast<AVFrame*>(frame), p);
            // Mixed reference/borrowed planes would become dangling because
            // av_frame_ref copies only completely non-reference-counted frames.
            if (!owner && frame->buf[0]) return false;
            if (owner) {
                const auto first = reinterpret_cast<uintptr_t>(frame->data[p]);
                const auto base = reinterpret_cast<uintptr_t>(owner->data);
                if (pitch < 0 && rowOffset > first) return false;
                const auto low = pitch < 0 ? first - uintptr_t(rowOffset) : first;
                if (low < base || low - base > owner->size || span > owner->size - (low - base)) return false;
            }
        }
        return true;
    }

    // m_mutex is held by all of these helpers.
    void DiscardPending() {
        if (m_pending) ++m_stats.purged;
        m_pending.reset();
    }
    void RequestStop() {
        m_accepting = false;
        m_stopped = true;
        ++m_epoch;
        DiscardPending();
    }
    // m_lifecycle is held by the external owner.
    void StopLocked() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            RequestStop();
        }
        m_ready.notify_all();
        if (m_worker.joinable()) m_worker.join();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_callback = {};
    }
    void Run() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_workerId = std::this_thread::get_id();
        }
        for (;;) {
            Frame frame;
            uint64_t generation = 0;
            Clock::time_point queued;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_ready.wait(lock, [this] { return m_stopped || bool(m_pending); });
                if (m_stopped) break;
                frame = std::move(m_pending);
                generation = m_pendingGeneration;
                queued = m_pendingAt;
                m_inFlight = true;
                m_stats.maxOutstanding = (std::max)(m_stats.maxOutstanding, 1u);
            }
            const double waitMs = std::chrono::duration<double, std::milli>(Clock::now() - queued).count();
            bool failed = false;
            try { m_callback(generation, frame.get(), waitMs); }
            catch (...) { failed = true; }
            // Release the frame before clearing inFlight, so the bound includes
            // owned queue storage until its last callback reference is released.
            frame.reset();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_stats.delivered;
                if (failed) ++m_stats.callbackErrors;
                m_inFlight = false;
            }
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_workerId = {};
    }
};

} // namespace PrismForge::Video