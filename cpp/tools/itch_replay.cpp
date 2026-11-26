// itch_replay: rebuilds the order books of chosen symbols from an ITCH 5.0
// file on the matching engine, checking invariants on every message.
//
//   build/itch_replay --symbols QQQ,SPY,AAPL data/day.NASDAQ_ITCH50
//   gzip -dc data/day.NASDAQ_ITCH50.gz | build/itch_replay --symbols AAPL -
//   build/itch_replay --symbols AAPL --tob build/aapl-tob.csv data/slice.NASDAQ_ITCH50
//
// Exits 1 if any invariant is violated (see cpp/replay/replayer.hpp).
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "itch/reader.hpp"
#include "replay/replayer.hpp"

namespace {

int usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s --symbols A,B,... [--max-price DOLLARS] [--max-orders N] [--tob out.csv] <file | ->\n",
                 prog);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    replay::Config cfg;
    const char* path = nullptr;
    const char* tob_path = nullptr;
    for (int i = 1; i < argc; i++) {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--symbols") == 0 && has_value) {
            std::stringstream ss(argv[++i]);
            for (std::string s; std::getline(ss, s, ',');)
                if (!s.empty()) cfg.symbols.insert(s);
        } else if (std::strcmp(argv[i], "--max-price") == 0 && has_value) {
            cfg.max_price_cents = std::strtoll(argv[++i], nullptr, 10) * 100;
        } else if (std::strcmp(argv[i], "--max-orders") == 0 && has_value) {
            cfg.max_orders = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--tob") == 0 && has_value) {
            tob_path = argv[++i];
        } else if (path == nullptr && argv[i][0] != '-') {
            path = argv[i];
        } else if (path == nullptr && std::strcmp(argv[i], "-") == 0) {
            path = argv[i];
        } else {
            return usage(argv[0]);
        }
    }
    if (path == nullptr || cfg.symbols.empty() || cfg.max_price_cents < 1) return usage(argv[0]);

    replay::Replayer rp(cfg);
    FILE* tob = nullptr;
    if (tob_path != nullptr) {
        tob = std::fopen(tob_path, "w");
        if (tob == nullptr) {
            std::perror(tob_path);
            return 1;
        }
        rp.write_top_of_book(tob);
    }

    itch::StreamResult res;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        if (std::strcmp(path, "-") == 0) {
            res = itch::parse_stream(0, rp);
        } else {
            itch::MappedFile f(path);
            const itch::ParseResult r = itch::parse(f.bytes(), rp);
            res = {r.consumed, r.messages, r.status, r.bad_type, r.bad_length};
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (tob != nullptr) std::fclose(tob);

    uint64_t book_events = 0, violations = 0, found = 0;
    for (const auto& b : rp.books()) {
        if (!b) continue;
        found++;
        book_events += b->counts.events;
        violations += b->violations.total();
    }
    std::printf("%s: %" PRIu64 " messages in %.1f s (%.1f M msgs/s), %" PRIu64 " book events for %" PRIu64
                " of %zu symbols\n\n",
                path, res.messages, secs, double(res.messages) / secs / 1e6, book_events, found, cfg.symbols.size());

    std::printf("%-6s %9s %9s %8s %6s %8s %9s %8s | %6s %6s %6s %7s | %8s %8s | %10s %10s | %s\n", "symbol", "events",
                "adds", "exec", "execP", "cancel", "delete", "replace", "offgrd", ">cap", "cross", "outExec", "peak",
                "end", "bid", "ask", "violations");
    for (const auto& bp : rp.books()) {
        if (!bp) continue;
        const auto& b = *bp;
        const auto& c = b.counts;
        const auto& v = b.violations;
        const auto t = replay::Replayer::top(b.book);
        std::printf("%-6s %9" PRIu64 " %9" PRIu64 " %8" PRIu64 " %6" PRIu64 " %8" PRIu64 " %9" PRIu64 " %8" PRIu64
                    " | %6" PRIu64 " %6" PRIu64 " %6" PRIu64 " %7" PRIu64 " | %8" PRIu64 " %8zu | %10.2f %10.2f | ",
                    b.symbol.c_str(), c.events, c.adds, c.executes, c.executes_with_price, c.cancels, c.deletes,
                    c.replaces, c.off_grid, c.above_cap, c.would_cross, c.outside_executions, c.peak_resting,
                    mb_order_count(b.book) + b.outside.size(), double(t.bid) / 100, double(t.ask) / 100);
        if (v.total() == 0) {
            std::printf("0\n");
        } else {
            std::printf("unknown_ref=%" PRIu64 " duplicate_ref=%" PRIu64 " over_reduce=%" PRIu64
                        " exec_not_at_best=%" PRIu64 " engine_traded=%" PRIu64 " pool_full=%" PRIu64 "\n",
                        v.unknown_ref, v.duplicate_ref, v.over_reduce, v.exec_not_at_best, v.engine_traded,
                        v.pool_full);
        }
    }
    if (found < cfg.symbols.size()) std::printf("\n(some symbols were not in the stock directory)\n");

    if (res.status != itch::Status::Ok) {
        std::printf("\nSTOPPED at byte %" PRIu64 ": %s\n", res.bytes, itch::status_str(res.status));
        return 1;
    }
    return violations == 0 ? 0 : 1;
}
