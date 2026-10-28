#ifdef NDEBUG
#error Core tests require active assertions
#endif
#include "core.hpp"
#include "log_protocol.hpp"
#include <cassert>
#include <vector>
#include <string>
#include <deque>
#include <random>
#include <iostream>
using namespace cer4all;
using Bytes = std::vector<uint8_t>;
void collect(const uint8_t *p, size_t n, void *ctx) {
    auto &v = *static_cast<Bytes *>(ctx);
    v.insert(v.end(), p, p + n);
}
std::string text(const Bytes &b) {
    return std::string(b.begin(), b.end());
}
Bytes feedNoise(NoiseFilter &f, const std::string &s) {
    Bytes b;
    for (uint8_t c : s)
        f.feed(c, collect, &b);
    return b;
}
std::string line(unsigned values) {
    std::string s = ":";
    for (unsigned i = 0; i < values; ++i)
        s += std::to_string(i % 256) + " ";
    return s + "\r\n";
}
int main() {
    // Exact existing gateway framing, fragmented and batched command notifications.
    char header[128], command[32];
    logHeader(header, sizeof(header), 123, true, "0.1.1-dev", 456);
    assert(std::string(header) == "GWLOG1 123 rec=1 fw=0.1.1-dev up=456\n");
    const std::string cmds = "REC1\r\nREC0\nCLR\nBYE\nPING\n";
    for (size_t split = 1; split <= cmds.size(); ++split) {
        LogCommands parser;
        for (size_t at = 0; at < cmds.size(); at += split)
            parser.feed(reinterpret_cast<const uint8_t *>(cmds.data()+at), std::min(split, cmds.size()-at));
        for (const char *want : {"REC1", "REC0", "CLR", "BYE", "PING"}) {
            assert(parser.next(command)); assert(std::string(command) == want);
        }
        assert(!parser.next(command));
    }
    LogCommands parser;
    std::string malformedCommand(100, 'x'); malformedCommand += "REC1\nREC0\n";
    parser.feed(reinterpret_cast<const uint8_t *>(malformedCommand.data()), malformedCommand.size());
    assert(parser.next(command) && std::string(command) == "REC0");
    assert(!parser.next(command));

    assert(fromPart(3) == Model::Certabo);
    for (int p : {0x20, 0x21, 0x22})
        assert(fromPart(uint8_t(p)) == Model::Spectrum);
    for (int p : {0, 1, 2, 4, 5, 255})
        assert(fromPart(uint8_t(p)) == Model::Unknown);
    // Thousands of wraparounds, all-or-nothing writes and byte-exact ordering.
    Ring<257> ring;
    std::deque<uint8_t> expected;
    std::mt19937 random(31);
    for (unsigned trial = 0; trial < 20000; ++trial) {
        size_t n = random() % 91;
        Bytes in(n);
        for (auto &x : in)
            x = uint8_t(random());
        if (random() % 2) {
            bool ok = ring.push(in.data(), n);
            assert(ok == (n <= 257 - expected.size()));
            if (ok)
                expected.insert(expected.end(), in.begin(), in.end());
        } else {
            Bytes out(n);
            size_t got = ring.peek(out.data(), n);
            assert(got == std::min(n, expected.size()));
            for (size_t i = 0; i < got; ++i)
                assert(out[i] == expected[i]);
            ring.consume(got);
            while (got--)
                expected.pop_front();
        }
        assert(ring.size() == expected.size());
    }
    // A realistic burst of short LED writes must use actual bytes, retain
    // each command boundary and epoch, and fail atomically when truly full.
    MessageRing<16384> messages;
    uint8_t shortLed[8] = {};
    unsigned accepted = 0;
    for (unsigned i = 0; i < 2000; ++i) {
        shortLed[0] = uint8_t(i);
        if (!messages.push(shortLed, 8, i))
            break;
        ++accepted;
    }
    assert(accepted == 16384 / 14);
    assert(!messages.push(shortLed, 8, 9999));
    for (unsigned i = 0; i < accepted; ++i) {
        uint8_t read[512];
        size_t n = 0;
        uint32_t epoch = 0;
        assert(messages.pop(read, sizeof(read), n, epoch));
        assert(n == 8 && read[0] == uint8_t(i) && epoch == i);
    }
    assert(messages.size() == 0);
    // Mixed sizes force wraparound and include zero-length writes.
    for (unsigned i = 0; i < 4000; ++i) {
        Bytes data(i % 513, uint8_t(i));
        assert(messages.push(data.data(), data.size(), i));
        uint8_t read[512];
        size_t n = 0;
        uint32_t epoch = 0;
        assert(messages.pop(read, sizeof(read), n, epoch));
        assert(n == data.size() && epoch == i);
        assert(Bytes(read, read + n) == data);
    }
    assert(!messages.push(shortLed, 513, 0) && messages.size() == 0);
    // Position/Sentio framing, every split; no assumptions about notification sizes.
    for (unsigned values : {8u, 320u})
        for (size_t split = 1; split < 257; ++split) {
            Observer observer;
            std::string s = "garbage\r\n" + line(values);
            unsigned found = 0;
            for (size_t start = 0; start < s.size(); start += split)
                for (size_t j = start; j < std::min(start + split, s.size()); ++j) {
                    auto result = observer.feed(uint8_t(s[j]));
                    if (result) {
                        assert(result == values);
                        ++found;
                    }
                }
            assert(found == 1);
        }
    Observer bad;
    for (uint8_t c : std::string(":999 0 0 0 0 0 0 0\n"))
        assert(!bad.feed(c));
    uint8_t mask[8]{};
    mask[3] = 24;
    mask[4] = 24;
    uint8_t rgb[247];
    rgbFromMask(mask, rgb);
    unsigned lit = 0;
    for (int i = 0; i < 81; ++i) {
        assert(rgb[2 + 3 * i] == 0 && rgb[3 + 3 * i] == 0);
        if (rgb[4 + 3 * i]) {
            ++lit;
            assert(rgb[4 + 3 * i] == 204);
        }
    }
    assert(lit == 9 && rgb[0] == 255 && rgb[1] == 85 && rgb[245] == 13 && rgb[246] == 10);
    uint8_t all[8];
    memset(all, 255, 8);
    rgbFromMask(all, rgb);
    for (int i = 0; i < 81; ++i)
        assert(rgb[4 + 3 * i] == 204);
    rgbFromMask(mask, rgb);
    // Four hand-checked corners prevent a symmetric centre-only mapping test
    // from hiding a 180-degree rotation or a reversed rank/file axis.
    const int rawSquares[4] = {0, 7, 56, 63};
    const int corners[4][4] = {{70, 71, 79, 80}, {63, 64, 72, 73}, {7, 8, 16, 17}, {0, 1, 9, 10}};
    for (int k = 0; k < 4; ++k) {
        uint8_t one[8] = {};
        one[rawSquares[k] / 8] = uint8_t(1u << (rawSquares[k] % 8));
        uint8_t frame[247];
        rgbFromMask(one, frame);
        for (int i = 0; i < 81; ++i) {
            bool isLit = false;
            for (int j = 0; j < 4; ++j)
                isLit = isLit || i == corners[k][j];
            assert(frame[4 + i * 3] == (isLit ? 204 : 0));
        }
    }
    // Native mono is never guessed from ID bytes; all possible byte values pass.
    Bytes sequence(768);
    for (size_t i = 0; i < sequence.size(); ++i)
        sequence[i] = uint8_t(i);
    for (Model m : {Model::Unknown, Model::Certabo, Model::Sentio}) {
        Leds l;
        Bytes out;
        l.feed(sequence.data(), sequence.size(), m, collect, &out);
        assert(out == sequence);
    }
    // Spectrum RGB reassembly for arbitrary fragment boundaries, duplicates suppressed.
    for (size_t chunk = 1; chunk <= 253; ++chunk) {
        Leds l;
        l.input = LedInput::Rgb;
        Bytes out;
        for (size_t i = 0; i < 247; i += chunk)
            l.feed(rgb + i, std::min(chunk, 247 - i), Model::Spectrum, collect, &out);
        assert(out == Bytes(rgb, rgb + 247));
        l.feed(rgb, 247, Model::Spectrum, collect, &out);
        assert(out.size() == 247);
    }
    for (size_t chunk : {size_t(20), size_t(253)}) {
        Leds l;
        Bytes out;
        for (size_t i = 0; i < 247; i += chunk)
            l.feed(rgb + i, std::min(chunk, 247 - i), Model::Spectrum, collect, &out);
        assert(out == Bytes(rgb, rgb + 247));
    }
    // 8-byte host writes translate, including an actual FF55 prefix in a mono mask.
    for (uint8_t first : {uint8_t(0), uint8_t(255)}) {
        mask[0] = first;
        mask[1] = 85;
        Leds l;
        Bytes out;
        l.feed(mask, 8, Model::Spectrum, collect, &out);
        rgbFromMask(mask, rgb);
        assert(out == Bytes(rgb, rgb + 247));
    }
    // Explicit mono mode handles fragmented/concatenated masks without heuristics.
    for (size_t chunk = 1; chunk <= 16; ++chunk) {
        Leds l;
        l.input = LedInput::Mono;
        Bytes out;
        Bytes data(mask, mask + 8);
        data.insert(data.end(), mask, mask + 8);
        for (size_t i = 0; i < data.size(); i += chunk)
            l.feed(data.data() + i, std::min(chunk, data.size() - i), Model::Spectrum, collect,
                   &out);
        assert(out == Bytes(rgb, rgb + 247));
    }
    Leds invalid;
    invalid.input = LedInput::Rgb;
    Bytes invalidOut;
    rgb[246] = 0;
    invalid.feed(rgb, 247, Model::Spectrum, collect, &invalidOut);
    assert(invalidOut.empty() && invalid.invalid == 1);
    rgb[246] = 10;
    invalid.reset();
    invalid.input = LedInput::Rgb;
    rgb[0] = 0;
    invalid.feed(rgb, 247, Model::Spectrum, collect, &invalidOut);
    assert(invalidOut.empty() && invalid.invalid == 1);
    rgb[0] = 255;
    // Learn arbitrary, user-owned tags from stability; NO prefix list.
    NoiseFilter noise;
    for (std::string id : {"5 0 195 33 252", "7 0 136 4 42", "93 12 1 2 3", "173 99 255 0 12"}) {
        noise.reset();
        for (int i = 0; i < 2; ++i)
            assert(text(feedNoise(noise, ":" + id + " \n")) == ":0 0 0 0 0 \n");
        assert(text(feedNoise(noise, ":" + id + " \n")) == ":" + id + " \n");
        // Known tags move immediately to another field, no 3-scan delay on moves.
        assert(text(feedNoise(noise, ":0 0 0 0 0 " + id + " \n")) == ":0 0 0 0 0 " + id + " \n");
    }
    noise.reset();
    for (int i = 0; i < 3; ++i)
        feedNoise(noise, ":173 99 255 0 12 \n");
    // Field-level latency: after FIVE values, before a complete 64-field record.
    Bytes partial;
    for (uint8_t c : std::string(":173 99 255 0 12 "))
        noise.feed(c, collect, &partial);
    assert(text(partial) == ":173 99 255 0 12 ");
    // Varying unknown readings hold the last known tag for at most three scans.
    for (int i = 0; i < 3; ++i)
        assert(text(feedNoise(noise, ":" + std::to_string(100 + i) + " 0 0 0 0 \n")) ==
               ":173 99 255 0 12 \n");
    assert(text(feedNoise(noise, ":103 0 0 0 0 \n")) == ":0 0 0 0 0 \n");
    // A real empty reading clears immediately, not after a hold timer.
    feedNoise(noise, ":173 99 255 0 12 \n");
    assert(text(feedNoise(noise, ":0 0 0 0 0 \n")) == ":0 0 0 0 0 \n");
    noise.reset();
    assert(text(feedNoise(noise, "D\r\n")) == "D\r\n");
    // Repeated phantom IDs on multiple fields must not become learned tags.
    noise.reset();
    for (int i = 0; i < 5; ++i)
        assert(text(feedNoise(noise, ":117 33 8 0 1 117 33 8 0 1 \n")) ==
               ":0 0 0 0 0 0 0 0 0 0 \n");
    // Actual upstream router: every byte of the FIRST Sentio line survives,
    // even if its USB chip initially suggested Spectrum. No learned fake IDs.
    for (Model hint : {Model::Unknown, Model::Certabo, Model::Spectrum})
        for (size_t chunk = 1; chunk <= 64; ++chunk) {
            PositionPipe pipe;
            pipe.reset(hint);
            Bytes out;
            std::string stream = ":128 1 255 0 34 56 78 90\n:0 1 2 3 4 5 6 7\n";
            for (size_t i = 0; i < stream.size(); i += chunk)
                pipe.feed(reinterpret_cast<const uint8_t *>(stream.data() + i),
                          std::min(chunk, stream.size() - i), true, collect, &out);
            assert(text(out) == stream && pipe.type() == Model::Sentio && pipe.frames() == 0);
        }
    // L/D acknowledgements may be interleaved with a position stream.
    Observer acknowledgements;
    unsigned observed = 0;
    for (uint8_t c : std::string(":0 1 D2 3 L4 5 6 7\n"))
        observed += acknowledgements.feed(c);
    assert(observed == 8);
    // Full normal-board protocol is byte-identical, including L/D and whitespace.
    for (Model hint : {Model::Unknown, Model::Certabo}) {
        PositionPipe pipe;
        pipe.reset(hint);
        Bytes out;
        std::string stream = "L\r\n" + line(320) + line(320);
        pipe.feed(reinterpret_cast<const uint8_t *>(stream.data()), stream.size(), true, collect,
                  &out);
        assert(text(out) == stream && pipe.frames() == 2 && pipe.type() == hint);
    }
    // Spectrum passthrough with the optional temporal filter disabled.
    PositionPipe pipe;
    pipe.reset(Model::Spectrum);
    Bytes raw;
    std::string stream = "D\r\n" + line(320) + line(320);
    pipe.feed(reinterpret_cast<const uint8_t *>(stream.data()), stream.size(), false, collect,
              &raw);
    assert(text(raw) == stream && pipe.frames() == 2);
    std::cout << "PASS: chip detection, queue wrap/overflow, 8/320 framing, RGB mapping, all "
                 "fragment sizes, native passthrough, dedup, field streaming and noise hold\n";
}
