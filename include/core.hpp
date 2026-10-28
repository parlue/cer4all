#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace cer4all {
enum class Model : uint8_t { Unknown, Certabo, Spectrum, Sentio };
inline const char *name(Model m) {
    switch (m) {
    case Model::Certabo:
        return "Certabo";
    case Model::Spectrum:
        return "Spectrum";
    case Model::Sentio:
        return "Sentio";
    default:
        return "Unknown";
    }
}
// Silicon Labs GET_PARTNUM, not product-string guesses and never tag prefixes.
inline Model fromPart(uint8_t p) {
    return p == 0x03 ? Model::Certabo : (p >= 0x20 && p <= 0x22 ? Model::Spectrum : Model::Unknown);
}
template <size_t N> class Ring {
    std::array<uint8_t, N> data_{};
    size_t read_ = 0, size_ = 0;

  public:
    size_t size() const {
        return size_;
    }
    size_t free() const {
        return N - size_;
    }
    bool push(const uint8_t *p, size_t n) {
        if (n > free())
            return false;
        for (size_t i = 0; i < n; ++i)
            data_[(read_ + size_ + i) % N] = p[i];
        size_ += n;
        return true;
    }
    size_t peek(uint8_t *p, size_t n) const {
        n = std::min(n, size_);
        for (size_t i = 0; i < n; ++i)
            p[i] = data_[(read_ + i) % N];
        return n;
    }
    void consume(size_t n) {
        n = std::min(n, size_);
        read_ = (read_ + n) % N;
        size_ -= n;
    }
    void clear() {
        read_ = size_ = 0;
    }
};
// Variable-size commands retain their write boundaries without reserving 512
// bytes for each common 8-byte LED mask. Caller supplies synchronization.
template <size_t N> class MessageRing {
    Ring<N> bytes_;

  public:
    bool push(const uint8_t *p, size_t n, uint32_t epoch) {
        if (n > 512 || bytes_.free() < n + 6)
            return false;
        uint8_t header[6] = {uint8_t(n),          uint8_t(n >> 8),      uint8_t(epoch),
                             uint8_t(epoch >> 8), uint8_t(epoch >> 16), uint8_t(epoch >> 24)};
        bytes_.push(header, 6);
        bytes_.push(p, n);
        return true;
    }
    bool pop(uint8_t *p, size_t capacity, size_t &n, uint32_t &epoch) {
        uint8_t header[6];
        if (bytes_.peek(header, 6) != 6)
            return false;
        n = size_t(header[0]) | (size_t(header[1]) << 8);
        if (n > capacity || bytes_.size() < n + 6)
            return false;
        epoch = uint32_t(header[2]) | (uint32_t(header[3]) << 8) | (uint32_t(header[4]) << 16) |
                (uint32_t(header[5]) << 24);
        bytes_.consume(6);
        bytes_.peek(p, n);
        bytes_.consume(n);
        return true;
    }
    void clear() {
        bytes_.clear();
    }
    size_t size() const {
        return bytes_.size();
    }
};
class Observer {
    bool active_ = false, digit_ = false, valid_ = true;
    unsigned count_ = 0, value_ = 0;

  public:
    void reset() {
        *this = Observer{};
    }
    // Recognize a completed 8- or 320-value record, independently of USB chunks.
    unsigned feed(uint8_t c) {
        if (c == 'L' || c == 'D')
            return 0;
        if (c == ':') {
            active_ = true;
            digit_ = false;
            valid_ = true;
            count_ = value_ = 0;
            return 0;
        }
        if (!active_)
            return 0;
        if (c >= '0' && c <= '9') {
            digit_ = true;
            value_ = value_ * 10 + c - '0';
            if (value_ > 255) {
                valid_ = false;
                value_ = 256;
            }
            return 0;
        }
        if (c == ' ' || c == '\r' || c == '\n') {
            if (digit_) {
                ++count_;
                if (value_ > 255)
                    valid_ = false;
                digit_ = false;
                value_ = 0;
            }
            if (c == '\n') {
                active_ = false;
                return valid_ && (count_ == 8 || count_ == 320) ? count_ : 0;
            }
            return 0;
        }
        active_ = false;
        return 0;
    }
};
using Emit = void (*)(const uint8_t *, size_t, void *);
inline void rgbFromMask(const uint8_t mask[8], uint8_t frame[247]) {
    std::memset(frame, 0, 247);
    frame[0] = 255;
    frame[1] = 85;
    frame[245] = 13;
    frame[246] = 10;
    for (unsigned i = 0; i < 64; ++i)
        if (mask[i / 8] & (1u << (i % 8))) {
            unsigned corner = (7 - i / 8) * 9 + 7 - i % 8;
            for (unsigned c : {corner, corner + 1, corner + 9, corner + 10})
                frame[4 + c * 3] = 204;
        }
}
enum class LedInput : uint8_t { Auto, Mono, Rgb };
class Leds {
    std::array<uint8_t, 247> pending_{};
    size_t used_ = 0;
    bool rgb_ = false;
    void complete(Model model, Emit out, void *ctx) {
        uint8_t converted[247];
        const uint8_t *bytes = pending_.data();
        size_t n = used_;
        if (rgb_) {
            if (n != 247 || bytes[0] != 255 || bytes[1] != 85 || bytes[245] != 13 ||
                bytes[246] != 10) {
                ++invalid;
                used_ = 0;
                return;
            }
        }
        if (model == Model::Spectrum && !rgb_) {
            rgbFromMask(bytes, converted);
            bytes = converted;
            n = 247;
        }
        // Preserve every completed command, including refreshes and blink repetitions.
        out(bytes, n, ctx);
        used_ = 0;
    }

  public:
    LedInput input = LedInput::Auto;
    uint32_t invalid = 0;
    void reset() {
        used_ = 0;
        rgb_ = false;
        invalid = 0;
    }
    bool pending() const {
        return used_ != 0;
    }
    void feed(const uint8_t *p, size_t n, Model model, Emit out, void *ctx) {
        // Mono/unknown/Sentio native streams stay byte-identical (including Sentio v2).
        if (model != Model::Spectrum) {
            out(p, n, ctx);
            return;
        }
        const bool wholeMono =
            input == LedInput::Mono || (input == LedInput::Auto && used_ == 0 && n == 8);
        for (size_t i = 0; i < n; ++i) {
            if (used_ == 0)
                rgb_ = input == LedInput::Rgb || (!wholeMono && p[i] == 255);
            pending_[used_++] = p[i];
            if (input != LedInput::Rgb && rgb_ && used_ == 2 && pending_[1] != 85)
                rgb_ = false;
            if (used_ == (rgb_ ? 247u : 8u))
                complete(model, out, ctx);
        }
    }
};
// USB-side protocol routing, shared by firmware and host regression tests.
class PositionPipe {
    Observer observer_;
    Model type_ = Model::Unknown;
    uint32_t frames_ = 0;

  public:
    void reset(Model type) {
        observer_.reset();
        type_ = type;
        frames_ = 0;
    }
    Model type() const {
        return type_;
    }
    uint32_t frames() const {
        return frames_;
    }
    void feed(const uint8_t *bytes, size_t n, Emit emit, void *ctx) {
        // Observation may select Sentio, but never edits or delays the stream.
        for (size_t i = 0; i < n; ++i) {
            const auto c = bytes[i];
            unsigned complete = observer_.feed(c);
            if (complete == 8 && type_ != Model::Sentio) {
                type_ = Model::Sentio;
            }
            if (complete == 320)
                ++frames_;
        }
        emit(bytes, n, ctx);
    }
};
} // namespace cer4all
