// Rebuilds NASDAQ order books from ITCH 5.0 on the matching engine.
//
// ITCH reports a book that NASDAQ already matched, so the replayer never lets
// the engine match. Each message maps onto the book like this:
//
//   Add (A/F)             mb_limit; it must rest without trading
//   Executed (E)          mb_reduce; the order must be at the best price on its side
//   Executed w/ price (C) mb_reduce; the price can differ (e.g. the opening cross)
//   Cancel (X)            mb_reduce
//   Delete (D)            mb_cancel
//   Replace (U)           mb_cancel the old ref, then add the new ref on the same
//                         side (mb_lookup supplies the side; U doesn't carry it)
//
// The engine indexes price levels directly in whole cents up to a cap, so some
// orders can't go in its book. Those are kept in a side table, counted by
// reason, and still checked when later messages refer to them:
//
//   off_grid     not a positive whole cent (legal under $1)
//   above_cap    above --max-price, e.g. $199,999.99 placeholder quotes
//   would_cross  would lock or cross the book, so the engine would trade it
//
// Everything in Violations should be zero on correct data and a correct
// replayer; the tool exits nonzero otherwise.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "book.h"
#include "itch/itch.hpp"

namespace replay {

struct Config {
    int64_t max_price_cents = 200'000;  // $2,000
    size_t max_orders = size_t(1) << 19;
    std::unordered_set<std::string> symbols;
};

struct Counts {
    uint64_t events = 0, adds = 0, executes = 0, executes_with_price = 0, cancels = 0, deletes = 0, replaces = 0;
    // Orders kept outside the engine's book, by reason.
    uint64_t off_grid = 0, above_cap = 0, would_cross = 0;
    uint64_t outside_executions = 0;  // executions of those orders
    uint64_t peak_resting = 0;
};

struct Violations {
    uint64_t unknown_ref = 0;       // a message refers to an order we don't have
    uint64_t duplicate_ref = 0;     // an add reuses a resting order's ref
    uint64_t over_reduce = 0;       // executes/cancels more shares than remain
    uint64_t exec_not_at_best = 0;  // an E execution of an order not at the best price
    uint64_t engine_traded = 0;     // the engine matched something during replay
    uint64_t pool_full = 0;         // --max-orders too small
    uint64_t total() const {
        return unknown_ref + duplicate_ref + over_reduce + exec_not_at_best + engine_traded + pool_full;
    }
};

struct TopOfBook {
    int64_t bid = 0, ask = 0;  // cents, 0 when empty
    uint64_t bid_qty = 0, ask_qty = 0;
    bool operator==(const TopOfBook&) const = default;
};

class Replayer {
public:
    struct Book {
        std::string symbol;
        mb_book* book = nullptr;
        Counts counts;
        Violations violations;
        TopOfBook last_top;
        struct Outside {
            mb_side side;
            uint32_t price;  // ITCH units (4 decimals), since it may not fit the book
            uint32_t qty;
        };
        std::unordered_map<uint64_t, Outside> outside;
    };

    explicit Replayer(Config cfg) : cfg_(std::move(cfg)), by_locate_(65536) {}
    ~Replayer() {
        for (auto& b : by_locate_)
            if (b) mb_book_destroy(b->book);
    }
    Replayer(const Replayer&) = delete;
    Replayer& operator=(const Replayer&) = delete;

    // Writes "timestamp,symbol,bid,bid_qty,ask,ask_qty" whenever a tracked
    // book's top changes; tools/itch_book_reference.py writes the same format.
    void write_top_of_book(FILE* f) { tob_ = f; }

    const std::vector<std::unique_ptr<Book>>& books() const { return by_locate_; }

    // ---- itch handler interface ----

    void on_directory(const itch::StockDirectory& d) {
        const std::string sym(d.symbol());
        if (!cfg_.symbols.count(sym) || by_locate_[d.locate]) return;
        auto b = std::make_unique<Book>();
        b->symbol = sym;
        b->book = mb_book_create(cfg_.max_price_cents, cfg_.max_orders, &Replayer::on_trade, b.get());
        if (b->book == nullptr) throw std::runtime_error("mb_book_create failed for " + sym);
        by_locate_[d.locate] = std::move(b);
    }

    void on_other(char, const unsigned char*, size_t) {}

    void on_event(const itch::Event& e) {
        Book* b = by_locate_[e.locate].get();
        if (b == nullptr) return;
        b->counts.events++;
        switch (e.type) {
            case itch::EventType::Add:
                b->counts.adds++;
                add(*b, e.ref, e.side == 'B' ? MB_BUY : MB_SELL, e.price, e.shares);
                break;
            case itch::EventType::Execute:
                b->counts.executes++;
                reduce(*b, e.ref, e.shares, /*check_best=*/true);
                break;
            case itch::EventType::ExecuteWithPrice:
                b->counts.executes_with_price++;
                reduce(*b, e.ref, e.shares, false);
                break;
            case itch::EventType::Cancel:
                b->counts.cancels++;
                reduce(*b, e.ref, e.shares, false, /*is_execution=*/false);
                break;
            case itch::EventType::Delete:
                b->counts.deletes++;
                remove(*b, e.ref);
                break;
            case itch::EventType::Replace: {
                b->counts.replaces++;
                mb_side side;
                if (mb_lookup(b->book, e.ref, &side, nullptr, nullptr)) {
                    mb_cancel(b->book, e.ref);
                } else if (auto it = b->outside.find(e.ref); it != b->outside.end()) {
                    side = it->second.side;
                    b->outside.erase(it);
                } else {
                    b->violations.unknown_ref++;
                    break;
                }
                add(*b, e.new_ref, side, e.price, e.shares);
                break;
            }
        }
        const uint64_t resting = mb_order_count(b->book) + b->outside.size();
        if (resting > b->counts.peak_resting) b->counts.peak_resting = resting;
        if (tob_ != nullptr) emit_top(*b, e.timestamp);
    }

    static TopOfBook top(const mb_book* book) {
        TopOfBook t;
        t.bid = mb_best_bid(book);
        t.ask = mb_best_ask(book);
        t.bid_qty = mb_depth(book, MB_BUY, t.bid);
        t.ask_qty = mb_depth(book, MB_SELL, t.ask);
        return t;
    }

private:
    static void on_trade(void* ctx, const mb_trade*) { static_cast<Book*>(ctx)->violations.engine_traded++; }

    void add(Book& b, uint64_t ref, mb_side side, uint32_t price, uint32_t qty) {
        if (mb_lookup(b.book, ref, nullptr, nullptr, nullptr) || b.outside.count(ref)) {
            b.violations.duplicate_ref++;
            return;
        }
        const int64_t cents = price / 100;
        if (price % 100 != 0 || cents < 1) {
            b.counts.off_grid++;
            b.outside.emplace(ref, Book::Outside{side, price, qty});
            return;
        }
        if (cents > cfg_.max_price_cents) {
            b.counts.above_cap++;
            b.outside.emplace(ref, Book::Outside{side, price, qty});
            return;
        }
        const int64_t opposite = side == MB_BUY ? mb_best_ask(b.book) : mb_best_bid(b.book);
        if (opposite != 0 && (side == MB_BUY ? cents >= opposite : cents <= opposite)) {
            b.counts.would_cross++;
            b.outside.emplace(ref, Book::Outside{side, price, qty});
            return;
        }
        const mb_status st = mb_limit(b.book, ref, side, cents, qty);
        if (st == MB_REJECT_FULL) b.violations.pool_full++;
        else if (st != MB_OK) b.violations.duplicate_ref++;  // the only other way a valid add fails
    }

    void reduce(Book& b, uint64_t ref, uint32_t qty, bool check_best, bool is_execution = true) {
        mb_side side;
        int64_t price;
        uint32_t remaining;
        if (mb_lookup(b.book, ref, &side, &price, &remaining)) {
            if (qty > remaining) {
                b.violations.over_reduce++;
                return;
            }
            if (check_best && price != (side == MB_BUY ? mb_best_bid(b.book) : mb_best_ask(b.book)))
                b.violations.exec_not_at_best++;
            mb_reduce(b.book, ref, qty);
            return;
        }
        auto it = b.outside.find(ref);
        if (it == b.outside.end()) {
            b.violations.unknown_ref++;
            return;
        }
        if (qty > it->second.qty) {
            b.violations.over_reduce++;
            return;
        }
        if (is_execution) b.counts.outside_executions++;
        it->second.qty -= qty;
        if (it->second.qty == 0) b.outside.erase(it);
    }

    void remove(Book& b, uint64_t ref) {
        if (mb_cancel(b.book, ref) == MB_OK) return;
        if (b.outside.erase(ref) == 0) b.violations.unknown_ref++;
    }

    void emit_top(Book& b, uint64_t ts) {
        const TopOfBook t = top(b.book);
        if (t == b.last_top) return;
        b.last_top = t;
        std::fprintf(tob_, "%llu,%s,%lld,%llu,%lld,%llu\n", (unsigned long long) ts, b.symbol.c_str(),
                     (long long) t.bid, (unsigned long long) t.bid_qty, (long long) t.ask,
                     (unsigned long long) t.ask_qty);
    }

    Config cfg_;
    std::vector<std::unique_ptr<Book>> by_locate_;
    FILE* tob_ = nullptr;
};

}  // namespace replay
