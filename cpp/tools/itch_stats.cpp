// itch_stats: decodes a whole ITCH 5.0 file and summarizes what's in it.
//
//   build/itch_stats data/day.NASDAQ_ITCH50           # mmap a decompressed file
//   gunzip -c data/day.NASDAQ_ITCH50.gz | build/itch_stats -
//
// Reports message counts by type, the busiest symbols by book events (to pick
// symbols for replay), and data checks the order book replayer depends on:
// prices that aren't whole cents, sides other than B/S, timestamps that go
// backwards. Throughput is only meaningful for the mmap path; through a pipe
// it measures gunzip.
#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "itch/itch.hpp"
#include "itch/reader.hpp"

namespace {

const char* type_name(char t) {
    switch (t) {
        case 'S': return "system event";
        case 'R': return "stock directory";
        case 'H': return "trading action";
        case 'Y': return "Reg SHO restriction";
        case 'L': return "market participant position";
        case 'V': return "MWCB decline level";
        case 'W': return "MWCB status";
        case 'K': return "IPO quoting period";
        case 'J': return "LULD auction collar";
        case 'h': return "operational halt";
        case 'A': return "add order";
        case 'F': return "add order (MPID)";
        case 'E': return "order executed";
        case 'C': return "order executed with price";
        case 'X': return "order cancel";
        case 'D': return "order delete";
        case 'U': return "order replace";
        case 'P': return "trade (hidden order)";
        case 'Q': return "cross trade";
        case 'B': return "broken trade";
        case 'I': return "net order imbalance";
        case 'N': return "retail price improvement";
        default: return "unknown";
    }
}

struct PerSymbol {
    uint64_t events = 0;
    uint64_t adds = 0;
};

struct Stats {
    std::array<uint64_t, 256> by_type{};
    std::vector<PerSymbol> by_locate = std::vector<PerSymbol>(65536);
    std::vector<std::string> symbol = std::vector<std::string>(65536);
    uint64_t prices_checked = 0;  // add and replace prices
    uint64_t sub_dollar = 0;      // < $1, where sub-penny prices are allowed
    uint64_t non_penny = 0;       // >= $1 but not a whole cent
    uint64_t bad_side = 0;
    uint32_t max_price = 0;
    uint64_t first_ts = UINT64_MAX, last_ts = 0, ts_backwards = 0;

    void saw_timestamp(uint64_t ts) {
        if (first_ts == UINT64_MAX) first_ts = ts;
        if (ts < last_ts) ts_backwards++;
        last_ts = std::max(last_ts, ts);
    }
    void check_price(uint32_t price) {
        prices_checked++;
        max_price = std::max(max_price, price);
        if (price < 10000) sub_dollar++;
        else if (price % 100 != 0) non_penny++;
    }

    void on_event(const itch::Event& e) {
        by_type[static_cast<unsigned char>(e.msg_type)]++;
        saw_timestamp(e.timestamp);
        PerSymbol& s = by_locate[e.locate];
        s.events++;
        if (e.type == itch::EventType::Add) {
            s.adds++;
            if (e.side != 'B' && e.side != 'S') bad_side++;
            check_price(e.price);
        } else if (e.type == itch::EventType::Replace) {
            check_price(e.price);
        }
    }
    void on_directory(const itch::StockDirectory& d) {
        by_type['R']++;
        saw_timestamp(d.timestamp);
        symbol[d.locate] = std::string(d.symbol());
    }
    void on_other(char t, const unsigned char* msg, size_t) {
        by_type[static_cast<unsigned char>(t)]++;
        saw_timestamp(itch::be48(msg + 5));
    }
};

std::string clock_time(uint64_t ns) {
    const uint64_t s = ns / 1'000'000'000;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%09" PRIu64, s / 3600, s / 60 % 60,
                  s % 60, ns % 1'000'000'000);
    return buf;
}

double pct(uint64_t part, uint64_t whole) { return whole ? 100.0 * double(part) / double(whole) : 0.0; }

void report(const char* path, bool piped, const itch::StreamResult& res, const Stats& st, double secs, size_t top) {
    std::printf("%s\n", path);
    std::printf("  %" PRIu64 " messages, %.2f GB in %.1f s: %.1f M msgs/s, %.0f MB/s%s\n\n", res.messages,
                double(res.bytes) / 1e9, secs, double(res.messages) / secs / 1e6, double(res.bytes) / secs / 1e6,
                piped ? " (through a pipe, so this is mostly gunzip)" : "");

    std::printf("  %-4s %-28s %14s %7s\n", "type", "message", "count", "share");
    std::vector<int> types;
    for (int t = 0; t < 256; t++)
        if (st.by_type[size_t(t)] > 0) types.push_back(t);
    std::sort(types.begin(), types.end(), [&](int a, int b) { return st.by_type[size_t(a)] > st.by_type[size_t(b)]; });
    for (int t : types)
        std::printf("  %-4c %-28s %14" PRIu64 " %6.2f%%\n", t, type_name(char(t)), st.by_type[size_t(t)],
                    pct(st.by_type[size_t(t)], res.messages));

    uint64_t symbols = 0, active = 0;
    for (size_t i = 0; i < st.symbol.size(); i++) {
        if (!st.symbol[i].empty()) symbols++;
        if (st.by_locate[i].events > 0) active++;
    }
    std::printf("\n  %" PRIu64 " symbols in the directory, %" PRIu64 " with book events\n", symbols, active);
    std::printf("  timestamps %s to %s, %" PRIu64 " went backwards\n", clock_time(st.first_ts).c_str(),
                clock_time(st.last_ts).c_str(), st.ts_backwards);
    std::printf("  add/replace prices: %" PRIu64 " checked, %" PRIu64 " under $1 (%.3f%%), %" PRIu64
                " at $1+ not a whole cent, max $%.4f\n",
                st.prices_checked, st.sub_dollar, pct(st.sub_dollar, st.prices_checked), st.non_penny,
                st.max_price / 1e4);
    std::printf("  adds with a side other than B/S: %" PRIu64 "\n", st.bad_side);

    std::vector<size_t> loc(st.by_locate.size());
    std::iota(loc.begin(), loc.end(), size_t(0));
    top = std::min(top, loc.size());
    std::partial_sort(loc.begin(), loc.begin() + long(top), loc.end(),
                      [&](size_t a, size_t b) { return st.by_locate[a].events > st.by_locate[b].events; });
    std::printf("\n  busiest symbols by book events\n  %-8s %6s %12s %12s\n", "symbol", "locate", "events", "adds");
    for (size_t i = 0; i < top && st.by_locate[loc[i]].events > 0; i++) {
        const size_t l = loc[i];
        std::printf("  %-8s %6zu %12" PRIu64 " %12" PRIu64 "\n", st.symbol[l].empty() ? "?" : st.symbol[l].c_str(), l,
                    st.by_locate[l].events, st.by_locate[l].adds);
    }
}

int usage(const char* prog) {
    std::fprintf(stderr, "usage: %s [--top N] <file | ->\n", prog);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    size_t top = 15;
    const char* path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            top = std::strtoul(argv[++i], nullptr, 10);
        } else if (path == nullptr) {
            path = argv[i];
        } else {
            return usage(argv[0]);
        }
    }
    if (path == nullptr) return usage(argv[0]);
    const bool piped = std::strcmp(path, "-") == 0;

    Stats st;
    itch::StreamResult res;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        if (piped) {
            res = itch::parse_stream(0, st);
        } else {
            itch::MappedFile f(path);
            const itch::ParseResult r = itch::parse(f.bytes(), st);
            res = {r.consumed, r.messages, r.status, r.bad_type, r.bad_length};
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    report(path, piped, res, st, secs, top);
    if (res.status != itch::Status::Ok) {
        std::printf("\n  STOPPED at byte %" PRIu64 ": %s", res.bytes, itch::status_str(res.status));
        if (res.status == itch::Status::BadLength)
            std::printf(" ('%c' with length %u, spec says %zu)", res.bad_type, res.bad_length,
                        itch::message_length(res.bad_type));
        std::printf("\n");
        return 1;
    }
    return 0;
}
