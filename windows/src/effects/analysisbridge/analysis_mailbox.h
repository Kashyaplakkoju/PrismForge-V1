#pragma once

#include "analysis_types.h"
#include <mutex>
#include <deque>
#include <memory>
#include <atomic>

namespace PrismForge::Effects::AnalysisBridge {

/// <summary>
/// Thread-Safe Bounded Analysis Mailbox and Frame-Alignment Selector.
/// 
/// Coordinates asynchronous background AI/model inference producers with the
/// synchronous real-time video processing pipeline.
/// 
/// Enforces:
/// - Source generation matching: immediately rejects or flushes analysis from prior camera sessions.
/// - Latency & age boundaries: drops or marks analysis expired if older than maxAgeMs.
/// - Out-of-order and future timestamp rejection.
/// - Bounded memory: strict queue capacity limit with no memory leaks or lag accumulation.
/// </summary>
class AnalysisMailbox {
public:
    struct MailboxMetrics {
        uint64_t totalPosted = 0;
        uint64_t totalAlignedDelivered = 0;
        uint64_t totalDroppedOld = 0;
        uint64_t totalRejectedGeneration = 0;
        uint64_t totalExpired = 0;
        size_t currentQueueDepth = 0;
    };

    explicit AnalysisMailbox(const AnalysisMailboxConfig& config = AnalysisMailboxConfig());
    ~AnalysisMailbox();

    AnalysisMailbox(const AnalysisMailbox&) = delete;
    AnalysisMailbox& operator=(const AnalysisMailbox&) = delete;

    /// <summary>
    /// Resets the mailbox for a new camera source session.
    /// Flushes all queued and delivered results and sets the active source generation.
    /// </summary>
    void ResetSource(uint64_t newSourceGeneration);

    /// <summary>
    /// Posts a newly computed analysis bundle into the mailbox from an inference thread.
    /// </summary>
    bool PostAnalysis(std::shared_ptr<const AnalysisBundle> bundle);

    /// <summary>
    /// Synchronously retrieves age-bounded past analysis for a specific video frame.
    /// frameTimestampPts and AnalysisBundle::timestampPts are microseconds.
    /// Future PTS or sequence IDs are never delivered, including cached results.
    /// timestampCompleted is optional steady-clock milliseconds (0 = unavailable).
    /// </summary>
    std::shared_ptr<const AnalysisBundle> GetAlignedAnalysis(
        uint64_t sourceGeneration,
        uint64_t frameSequenceId,
        int64_t frameTimestampPts,
        AnalysisAlignmentStatus& status
    );

    /// <summary>
    /// Returns current mailbox operational metrics.
    /// </summary>
    MailboxMetrics GetMetrics() const;

    /// <summary>
    /// Updates configuration; clamps maxAgeMs to [0, 60000] and queue depth to [1, 64].
    /// </summary>
    void UpdateConfig(const AnalysisMailboxConfig& newConfig);

private:
    mutable std::mutex m_mutex;
    AnalysisMailboxConfig m_config;
    uint64_t m_currentSourceGeneration = 0;
    uint64_t m_lastPostedSequenceId = 0;

    std::deque<std::shared_ptr<const AnalysisBundle>> m_queue;
    std::shared_ptr<const AnalysisBundle> m_latestDelivered;

    MailboxMetrics m_metrics;
};

} // namespace PrismForge::Effects::AnalysisBridge
