#pragma once
#include <cstdio>
#include <cstring>
#include "core.hpp"
namespace cer4all {
// Same line protocol as eChessGw. Callers serialize access to the queue.
struct LogCommands {
    Ring<256> ready;
    char line[32]{};
    size_t used = 0;
    bool discard = false;
    void feed(const uint8_t *p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            if (p[i] == '\n') {
                if (!discard) {
                    while (used && (line[used-1] == '\r' || line[used-1] == ' ')) --used;
                    line[used++] = 0;
                    ready.push(reinterpret_cast<uint8_t *>(line), used);
                }
                used = 0; discard = false;
            } else if (used < sizeof(line)-1) line[used++] = char(p[i]);
            else discard = true;
        }
    }
    bool next(char *out) {
        size_t n = 0;
        uint8_t c;
        while (ready.peek(&c, 1)) {
            ready.consume(1);
            out[n++] = char(c);
            if (!c) return true;
        }
        return false;
    }
};
inline int logHeader(char *out, size_t max, size_t bytes, bool recording,
                     const char *firmware, unsigned long uptime) {
    return snprintf(out, max, "GWLOG1 %u rec=%d fw=%s up=%lu\n",
                    unsigned(bytes), recording ? 1 : 0, firmware, uptime);
}
}
