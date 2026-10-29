#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace cer4all {
// Observe a FIFO without storing or changing its data. Call under the FIFO lock.
// A marker samples the time from enqueue until all bytes ahead of it complete.
// This is sampled tail residence, not an exact oldest-byte or radio latency.
class QueueProbe {
    uint64_t queued_ = 0, completed_ = 0, marker_ = 0;
    uint32_t since_ = 0, maxMs_ = 0;
    size_t high_ = 0;
    bool sampling_ = false;
  public:
    void clear() { *this = QueueProbe{}; }
    size_t pending() const { return size_t(queued_ - completed_); }
    void push(size_t n, uint32_t now) {
        if (!n) return;
        queued_ += n;
        high_ = std::max(high_, pending());
        if (!sampling_) {
            marker_ = queued_;
            since_ = now;
            sampling_ = true;
        }
    }
    void complete(size_t n, uint32_t now) {
        completed_ += std::min(n, pending());
        if (sampling_ && completed_ >= marker_) {
            maxMs_ = std::max(maxMs_, uint32_t(now - since_));
            sampling_ = false;
        }
    }
    struct Window { size_t pending, high; uint32_t waitMs; };
    Window window(uint32_t now) {
        Window w{pending(), high_, maxMs_};
        if (sampling_) w.waitMs = std::max(w.waitMs, uint32_t(now - since_));
        high_ = pending();
        maxMs_ = 0;
        return w;
    }
};
} // namespace cer4all
