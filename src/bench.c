/*
 * Benchmark: replays a synthetic order flow (limits around a drifting mid
 * price, cancels of recent orders, small market orders) and reports
 * throughput plus per-operation latency percentiles.
 *
 *   bench [-n ops] [-s seed] [--csv out.csv]
 *
 * The workload is generated before timing starts. Throughput comes from one
 * untimed-per-op pass; latencies come from a second pass on a fresh book that
 * timestamps every operation.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "book.h"

#define MAX_PRICE 100000
#define LIVE_RING 4096

typedef enum { OP_LIMIT, OP_MARKET, OP_CANCEL, OP_KINDS } op_kind;
static const char *KIND_NAMES[OP_KINDS] = {"limit", "market", "cancel"};

typedef struct op {
    op_kind kind;
    mb_side side;
    uint64_t id;
    int64_t price;
    uint32_t qty;
} op;

static uint64_t rng_state;

static uint64_t rng(void) { /* xorshift64* */
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

static uint64_t rng_below(uint64_t n) { return rng() % n; }

static inline uint64_t now_ns(void) {
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
#endif
}

static op *generate(size_t n, uint64_t seed) {
    op *ops = malloc(n * sizeof *ops);
    if (ops == NULL) return NULL;
    rng_state = seed ? seed : 1;

    uint64_t live[LIVE_RING] = {0};
    size_t live_next = 0;
    uint64_t next_id = 1;
    int64_t mid = MAX_PRICE / 2;

    for (size_t i = 0; i < n; i++) {
        if (rng_below(100) < 10) { /* mid price random walk */
            mid += (int64_t) rng_below(3) - 1;
            if (mid < 1000) mid = 1000;
            if (mid > MAX_PRICE - 1000) mid = MAX_PRICE - 1000;
        }
        uint64_t roll = rng_below(100);
        op *o = &ops[i];
        o->side = rng_below(2) ? MB_BUY : MB_SELL;
        if (roll < 55) {
            o->kind = OP_LIMIT;
            o->id = next_id++;
            /* 85% passive (rests away from mid), 15% aggressive (crosses). */
            int64_t offset = 1 + (int64_t) rng_below(20);
            int aggressive = rng_below(100) < 15;
            int below = (o->side == MB_BUY) != aggressive;
            o->price = below ? mid - offset : mid + offset;
            o->qty = 1 + (uint32_t) rng_below(500);
            live[live_next++ % LIVE_RING] = o->id;
        } else if (roll < 90) {
            o->kind = OP_CANCEL;
            o->id = live[rng_below(LIVE_RING)];
            if (o->id == 0) o->id = 1;
        } else {
            o->kind = OP_MARKET;
            o->id = next_id++;
            o->qty = 1 + (uint32_t) rng_below(200);
        }
    }
    return ops;
}

static uint64_t trade_count;
static void count_trade(void *ctx, const mb_trade *t) {
    (void) ctx;
    (void) t;
    trade_count++;
}

static inline void apply(mb_book *b, const op *o) {
    switch (o->kind) {
        case OP_LIMIT: mb_limit(b, o->id, o->side, o->price, o->qty); break;
        case OP_MARKET: mb_market(b, o->id, o->side, o->qty, NULL); break;
        case OP_CANCEL: mb_cancel(b, o->id); break;
        default: break;
    }
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *) a, y = *(const uint32_t *) b;
    return (x > y) - (x < y);
}

static void report(const char *name, uint32_t *lat, size_t n) {
    if (n == 0) return;
    qsort(lat, n, sizeof *lat, cmp_u32);
#define PCT(p) lat[(size_t) ((double) (n - 1) * (p))]
    printf("  %-7s %10zu ops   p50 %5" PRIu32 " ns   p99 %6" PRIu32 " ns   p99.9 %6" PRIu32 " ns   max %8" PRIu32 " ns\n",
           name, n, PCT(0.50), PCT(0.99), PCT(0.999), lat[n - 1]);
#undef PCT
}

int main(int argc, char **argv) {
    size_t n = 5000000;
    uint64_t seed = 42;
    const char *csv_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) n = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) csv_path = argv[++i];
        else {
            fprintf(stderr, "usage: %s [-n ops] [-s seed] [--csv out.csv]\n", argv[0]);
            return 2;
        }
    }

    op *ops = generate(n, seed);
    uint32_t *lat = malloc(n * sizeof *lat);
    if (ops == NULL || lat == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    /* Pass 1: throughput. */
    mb_book *book = mb_book_create(MAX_PRICE, n, count_trade, NULL);
    uint64_t t0 = now_ns();
    for (size_t i = 0; i < n; i++) apply(book, &ops[i]);
    uint64_t elapsed = now_ns() - t0;
    size_t resting = mb_order_count(book);
    mb_book_destroy(book);

    printf("matchbook benchmark: %zu ops, seed %" PRIu64 "\n", n, seed);
    printf("  throughput %.2f M ops/s (%.1f ns/op avg), %" PRIu64 " trades, %zu orders resting at end\n",
           (double) n / ((double) elapsed / 1e9) / 1e6, (double) elapsed / (double) n, trade_count, resting);

    /* Pass 2: per-op latency. */
    book = mb_book_create(MAX_PRICE, n, count_trade, NULL);
    for (size_t i = 0; i < n; i++) {
        uint64_t s = now_ns();
        apply(book, &ops[i]);
        uint64_t d = now_ns() - s;
        lat[i] = d > UINT32_MAX ? UINT32_MAX : (uint32_t) d;
    }
    mb_book_destroy(book);

    if (csv_path != NULL) {
        FILE *f = fopen(csv_path, "w");
        if (f == NULL) {
            perror(csv_path);
            return 1;
        }
        fprintf(f, "seq,kind,side,latency_ns\n");
        for (size_t i = 0; i < n; i++) {
            fprintf(f, "%zu,%s,%c,%" PRIu32 "\n", i, KIND_NAMES[ops[i].kind], ops[i].side == MB_BUY ? 'B' : 'S', lat[i]);
        }
        fclose(f);
        printf("  wrote per-op latencies to %s\n", csv_path);
    }

    /* Split by kind, then report (report sorts in place). */
    uint32_t *by_kind[OP_KINDS];
    size_t counts[OP_KINDS] = {0};
    for (int k = 0; k < OP_KINDS; k++) by_kind[k] = malloc(n * sizeof(uint32_t));
    for (size_t i = 0; i < n; i++) by_kind[ops[i].kind][counts[ops[i].kind]++] = lat[i];
    printf("latency (includes ~%s timer overhead):\n", "20-40 ns");
    report("all", lat, n);
    for (int k = 0; k < OP_KINDS; k++) {
        report(KIND_NAMES[k], by_kind[k], counts[k]);
        free(by_kind[k]);
    }

    free(lat);
    free(ops);
    return 0;
}
