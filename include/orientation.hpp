#pragma once
#include "core.hpp"

namespace cer4all {
using Tag = std::array<uint8_t, 5>;
using PhysicalBoard = std::array<Tag, 64>;

// Learn only the known initial arrangement. No ID prefixes, position denoising,
// invented IDs or move interpretation. Spare pieces in the middle are ignored.
class BoardOrientation {
    PhysicalBoard reference_{}, candidate_{};
    unsigned learnHits_ = 0, turnHits_ = 0;
    bool learned_ = false, rotated_ = false;
    static bool occupied(const Tag &t) { return t != Tag{}; }
    static bool edge(unsigned i) { return i < 16 || i >= 48; }
    static char initial(unsigned i) {
        static constexpr char start[] = "rnbqkbnrpppppppp................................PPPPPPPPRNBQKBNR";
        return start[i];
    }
    static bool completeEdges(const PhysicalBoard &b) {
        for (unsigned i = 0; i < 64; ++i) if (edge(i)) {
            if (!occupied(b[i])) return false;
            for (unsigned j = 0; j < i; ++j)
                if (edge(j) && b[i] == b[j]) return false;
        }
        return true;
    }
    static bool sameEdges(const PhysicalBoard &a, const PhysicalBoard &b) {
        for (unsigned i = 0; i < 64; ++i) if (edge(i) && a[i] != b[i]) return false;
        return true;
    }
    char role(const Tag &tag) const {
        for (unsigned i = 0; i < 64; ++i)
            if (edge(i) && reference_[i] == tag) return initial(i);
        return '?';
    }
    bool matches(const PhysicalBoard &b, bool turn) const {
        for (unsigned i = 0; i < 64; ++i)
            if (edge(i) && role(b[i]) != initial(turn ? 63-i : i)) return false;
        return true;
    }
  public:
    bool rotated() const { return rotated_; }
    bool learned() const { return learned_; }
    void reset() { *this = BoardOrientation{}; }
    void invalid() { learnHits_ = turnHits_ = 0; }
    bool observe(const PhysicalBoard &b) {
        if (!completeEdges(b)) { invalid(); return false; }
        if (!learned_) {
            if (!learnHits_ || !sameEdges(candidate_, b)) {
                candidate_ = b; learnHits_ = 1;
            } else ++learnHits_;
            if (learnHits_ >= 3) { reference_ = b; learned_ = true; }
            return false;
        }
        if (!matches(b, !rotated_)) { turnHits_ = 0; return false; }
        if (++turnHits_ < 3) return false;
        rotated_ = !rotated_;
        turnHits_ = 0;
        return true;
    }
};

// Passive numeric record decoder; forwarding and orientation are separate.
class OrientationRecord {
    bool active_ = false, digit_ = false, valid_ = false;
    unsigned value_ = 0, count_ = 0;
  public:
    PhysicalBoard board{};
    bool active() const { return active_; }
    void reset() { *this = OrientationRecord{}; }
    // -1 malformed completed record; 8/320 valid record; 0 not complete.
    int feed(uint8_t c) {
        if (c == ':') {
            active_ = valid_ = true; digit_ = false; value_ = count_ = 0;
            return 0;
        }
        if (!active_) return 0;
        if (c == 'L' || c == 'D') return 0;
        if (c >= '0' && c <= '9') {
            digit_ = true;
            value_ = std::min(256u, value_ * 10 + c - '0');
            if (value_ > 255) valid_ = false;
        } else if (c == ' ' || c == '\r' || c == '\n') {
            if (digit_) {
                if (count_ < 320) board[count_/5][count_%5] = uint8_t(value_);
                if (++count_ > 320) valid_ = false;
                value_ = 0; digit_ = false;
            }
            if (c == '\n') {
                active_ = false;
                return valid_ && (count_ == 8 || count_ == 320) ? int(count_) : -1;
            }
        } else valid_ = false;
        return 0;
    }
};

using OrientationChanged = void (*)(bool, void *);
class OrientedPositionPipe {
    Model type_ = Model::Unknown;
    uint32_t frames_ = 0;
    BoardOrientation orientation_;
    OrientationRecord record_;
    uint8_t raw_[2048]{};
    char output_[1284]{}; // ':' + at most 320 * ('255' + space) + CRLF.
    size_t rawUsed_ = 0;
    bool holding_ = false, overflow_ = false;
    bool supportsOrientation() const { return type_ == Model::Certabo || type_ == Model::Spectrum; }
    void formattedRecord(Emit emit, void *ctx, bool turn, bool acknowledgements = true) {
        // Preserve acknowledgement bytes even if interspersed with a record.
        for (size_t i = 0; acknowledgements && i < rawUsed_; ++i)
            if (raw_[i] == 'L' || raw_[i] == 'D') emit(raw_+i, 1, ctx);
        size_t n = 0;
        output_[n++] = ':';
        for (unsigned square = 0; square < 64; ++square)
            for (uint8_t value : record_.board[turn ? 63-square : square]) {
                int written = snprintf(output_+n, sizeof(output_)-n, "%u ", unsigned(value));
                n += size_t(written);
            }
        if (!acknowledgements || (rawUsed_ >= 2 && raw_[rawUsed_-2] == '\r')) output_[n++] = '\r';
        output_[n++] = '\n';
        emit(reinterpret_cast<uint8_t *>(output_), n, ctx);
    }
  public:
    void reset(Model m) {
        type_ = m; frames_ = 0; orientation_.reset(); record_.reset();
        rawUsed_ = 0; holding_ = overflow_ = false;
    }
    Model type() const { return type_; }
    uint32_t frames() const { return frames_; }
    bool rotated() const { return orientation_.rotated(); }
    bool learned() const { return orientation_.learned(); }
    bool overflow() const { return overflow_; }
    void feed(const uint8_t *p, size_t n, Emit emit, void *ctx,
              OrientationChanged changed = nullptr) {
        size_t rawStart = 0;
        for (size_t i = 0; i < n && !overflow_; ++i) {
            const bool turn = orientation_.rotated();
            const uint8_t c = p[i];
            if (c == ':' && record_.active()) orientation_.invalid();
            if (turn) {
                if (c == ':') {
                    if (holding_) emit(raw_, rawUsed_, ctx);
                    holding_ = true; rawUsed_ = 0;
                }
                if (holding_) {
                    if (rawUsed_ == sizeof(raw_)) { overflow_ = true; break; }
                    raw_[rawUsed_++] = c;
                } else emit(p+i, 1, ctx);
            }
            int complete = record_.feed(c);
            if (complete == 320) ++frames_;
            if (complete == 8) type_ = Model::Sentio;
            if (complete != 0 && turn && holding_) {
                if (complete == 320) formattedRecord(emit, ctx, true);
                else emit(raw_, rawUsed_, ctx);
                holding_ = false; rawUsed_ = 0;
            }
            bool switched = false;
            if (complete == 320 && supportsOrientation()) switched = orientation_.observe(record_.board);
            else if (complete != 0) orientation_.invalid();
            if (switched) {
                if (!turn) emit(p+rawStart, i+1-rawStart, ctx);
                rawStart = i+1;
                if (changed) changed(orientation_.rotated(), ctx);
                // Immediately seed the new coordinate epoch with a complete
                // valid position, rather than waiting for the next USB scan.
                formattedRecord(emit, ctx, orientation_.rotated(), false);
            } else if (turn) rawStart = i+1;
        }
        if (!orientation_.rotated() && rawStart < n && !overflow_) emit(p+rawStart, n-rawStart, ctx);
    }
};

// Two short flashes. The USB worker owns it; no overlay survives its final OFF.
class OrientationBlink {
    unsigned phase_ = 4;
    uint32_t due_ = 0;
  public:
    bool active() const { return phase_ < 4; }
    void reset() { phase_ = 4; }
    void start(uint32_t now) { phase_ = 0; due_ = now; }
    bool next(uint32_t now, bool &on) {
        if (!active() || int32_t(now-due_) < 0) return false;
        on = (phase_++ % 2) == 0; due_ = now + 120;
        return true;
    }
};
} // namespace cer4all
