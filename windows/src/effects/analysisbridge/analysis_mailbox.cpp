#include "analysis_mailbox.h"
#include <chrono>
#include <algorithm>
#include <limits>

namespace PrismForge::Effects::AnalysisBridge {

namespace {

int64_t GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

AnalysisMailboxConfig BoundedConfig(AnalysisMailboxConfig config) {
    config.maxAgeMs = std::clamp(config.maxAgeMs, 0, 60000);
    config.maxQueueDepth = std::clamp(config.maxQueueDepth, 1, 64);
    return config;
}

// For ordered signed timestamps, unsigned subtraction represents the complete
// nonnegative distance even when the difference would overflow int64_t.
uint64_t TimestampDistance(int64_t later, int64_t earlier) {
    return static_cast<uint64_t>(later) - static_cast<uint64_t>(earlier);
}

} // anonymous namespace

AnalysisMailbox::AnalysisMailbox(const AnalysisMailboxConfig& config)
    : m_config(BoundedConfig(config)) {
}

AnalysisMailbox::~AnalysisMailbox() = default;

void AnalysisMailbox::ResetSource(uint64_t newSourceGeneration) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentSourceGeneration = newSourceGeneration;
    m_lastPostedSequenceId = 0;
    m_queue.clear();
    m_latestDelivered.reset();
    m_metrics.currentQueueDepth = 0;
}

void AnalysisMailbox::UpdateConfig(const AnalysisMailboxConfig& newConfig) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = BoundedConfig(newConfig);
    while (m_queue.size() > static_cast<size_t>(m_config.maxQueueDepth)) {
        m_queue.pop_front();
        ++m_metrics.totalDroppedOld;
    }
    m_metrics.currentQueueDepth = m_queue.size();
}

bool AnalysisMailbox::PostAnalysis(std::shared_ptr<const AnalysisBundle> bundle) {
    if (!bundle) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_metrics.totalPosted;
    if (bundle->sourceGeneration != m_currentSourceGeneration) {
        ++m_metrics.totalRejectedGeneration;
        return false;
    }
    if (m_config.dropStaleImmediately && bundle->sequenceId < m_lastPostedSequenceId) {
        ++m_metrics.totalDroppedOld;
        return false;
    }
    m_lastPostedSequenceId = std::max(m_lastPostedSequenceId, bundle->sequenceId);
    while (m_queue.size() >= static_cast<size_t>(m_config.maxQueueDepth)) {
        m_queue.pop_front();
        ++m_metrics.totalDroppedOld;
    }
    m_queue.push_back(bundle);
    m_metrics.currentQueueDepth = m_queue.size();
    return true;
}

std::shared_ptr<const AnalysisBundle> AnalysisMailbox::GetAlignedAnalysis(
    uint64_t sourceGeneration,
    uint64_t frameSequenceId,
    int64_t frameTimestampPts,
    AnalysisAlignmentStatus& status
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (sourceGeneration != m_currentSourceGeneration) {
        status = AnalysisAlignmentStatus::SourceGenerationMismatch;
        return nullptr;
    }

    const int64_t nowMs = GetCurrentTimeMs();
    const uint64_t maxAgeMs = static_cast<uint64_t>(m_config.maxAgeMs);
    const uint64_t maxAgePts = maxAgeMs * 1000ULL; // API PTS are microseconds.
    const auto expired = [&](const AnalysisBundle& bundle) {
        const bool ptsExpired = bundle.timestampPts <= frameTimestampPts &&
            TimestampDistance(frameTimestampPts, bundle.timestampPts) > maxAgePts;
        const bool wallExpired = bundle.timestampCompleted > 0 && bundle.timestampCompleted <= nowMs &&
            TimestampDistance(nowMs, bundle.timestampCompleted) > maxAgeMs;
        return ptsExpired || wallExpired;
    };
    bool sawExpired = false;
    bool sawFuturePts = false;
    bool sawFutureSequence = false;
    const auto eligible = [&](const AnalysisBundle& bundle) {
        if (bundle.timestampPts > frameTimestampPts ||
            (bundle.timestampCompleted > 0 && bundle.timestampCompleted > nowMs)) {
            sawFuturePts = true;
            return false;
        }
        if (bundle.sequenceId > frameSequenceId) {
            sawFutureSequence = true;
            return false;
        }
        return true;
    };

    // Timestamp order need not match insertion order. Examine the entire bounded
    // queue, retaining future results for a later eligible target frame.
    for (auto it = m_queue.begin(); it != m_queue.end();) {
        if (expired(**it)) {
            it = m_queue.erase(it);
            ++m_metrics.totalDroppedOld;
            sawExpired = true;
        } else {
            ++it;
        }
    }

    std::shared_ptr<const AnalysisBundle> bestCandidate;
    uint64_t minDiff = std::numeric_limits<uint64_t>::max();
    size_t bestIdx = 0;
    for (size_t i = 0; i < m_queue.size(); ++i) {
        const auto& bundle = m_queue[i];
        if (!eligible(*bundle)) continue;
        const uint64_t diff = TimestampDistance(frameTimestampPts, bundle->timestampPts);
        if (!bestCandidate || diff < minDiff ||
            (diff == minDiff && bundle->sequenceId > bestCandidate->sequenceId)) {
            minDiff = diff;
            bestCandidate = bundle;
            bestIdx = i;
        }
    }

    if (bestCandidate) {
        // Remove consumed/past entries through the selected result, but preserve
        // any future entry that an out-of-order producer inserted before it.
        auto consumed = m_queue.begin();
        size_t remaining = bestIdx + 1;
        while (remaining > 0) {
            if (eligible(**consumed)) {
                consumed = m_queue.erase(consumed);
            } else {
                ++consumed;
            }
            --remaining;
        }
        m_latestDelivered = bestCandidate;
        ++m_metrics.totalAlignedDelivered;
        m_metrics.currentQueueDepth = m_queue.size();
        status = AnalysisAlignmentStatus::Aligned;
        return bestCandidate;
    }

    if (m_latestDelivered) {
        if (expired(*m_latestDelivered)) {
            sawExpired = true;
        } else if (eligible(*m_latestDelivered)) {
            m_metrics.currentQueueDepth = m_queue.size();
            status = AnalysisAlignmentStatus::Aligned;
            return m_latestDelivered;
        }
    }

    m_metrics.currentQueueDepth = m_queue.size();
    if (sawFuturePts) {
        status = AnalysisAlignmentStatus::FutureResult;
    } else if (sawFutureSequence) {
        status = AnalysisAlignmentStatus::SequenceOutOfOrder;
    } else if (sawExpired) {
        status = AnalysisAlignmentStatus::Expired;
        ++m_metrics.totalExpired;
    } else {
        status = AnalysisAlignmentStatus::NotReady;
    }
    return nullptr;
}

AnalysisMailbox::MailboxMetrics AnalysisMailbox::GetMetrics() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    MailboxMetrics metrics = m_metrics;
    metrics.currentQueueDepth = m_queue.size();
    return metrics;
}

} // namespace PrismForge::Effects::AnalysisBridge