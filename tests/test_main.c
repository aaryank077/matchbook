#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "book.h"
#include "idmap.h"
#include "journal.h"
#include "list.h"
#include "pool.h"
#include "test.h"

int tests_run, tests_failed, checks_failed;

/* ---- trade capture ---------------------------------------------------- */

#define MAX_TRADES 64
static mb_trade trades[MAX_TRADES];
static int ntrades;

static void capture(void *ctx, const mb_trade *t) {
    (void) ctx;
    if (ntrades < MAX_TRADES) trades[ntrades] = *t;
    ntrades++;
}

static mb_book *fresh_book(void) {
    ntrades = 0;
    return mb_book_create(1000, 100, capture, NULL);
}

/* ---- list / pool / idmap --------------------------------------------- */

typedef struct item { int v; list_elem elem; } item;

static void test_list_fifo_and_remove(void) {
    list l;
    list_init(&l);
    item a = {.v = 1}, b = {.v = 2}, c = {.v = 3};
    CHECK(list_empty(&l));
    list_push_back(&l, &a.elem);
    list_push_back(&l, &b.elem);
    list_push_back(&l, &c.elem);
    list_remove(&b.elem);
    CHECK_EQ(list_entry(list_front(&l), item, elem)->v, 1);
    list_remove(&a.elem);
    CHECK_EQ(list_entry(list_front(&l), item, elem)->v, 3);
    list_remove(&c.elem);
    CHECK(list_empty(&l));
}

static void test_pool_exhaustion_and_reuse(void) {
    pool p;
    CHECK_EQ(pool_init(&p, 24, 3), 0);
    void *a = pool_alloc(&p), *b = pool_alloc(&p), *c = pool_alloc(&p);
    CHECK(a && b && c && a != b && b != c);
    CHECK(pool_alloc(&p) == NULL);
    pool_free(&p, b);
    CHECK_EQ(pool_alloc(&p), b);
    CHECK_EQ(p.in_use, 3u);
    pool_destroy(&p);
}

/* Random inserts/removes checked against a plain array, to exercise backward-shift deletion. */
static void test_idmap_matches_naive_model(void) {
    enum { N = 2000 };
    idmap m;
    CHECK_EQ(idmap_init(&m, N), 0);
    static int present[N + 1];
    memset(present, 0, sizeof present);
    srand(7);
    for (int step = 0; step < 200000; step++) {
        uint64_t k = 1 + (uint64_t) (rand() % N);
        if (rand() % 2) {
            int r = idmap_put(&m, k, (void *) (uintptr_t) k);
            CHECK_EQ(r, present[k]);
            present[k] = 1;
        } else {
            void *v = idmap_remove(&m, k);
            CHECK_EQ(v, present[k] ? (void *) (uintptr_t) k : NULL);
            present[k] = 0;
        }
    }
    size_t count = 0;
    for (uint64_t k = 1; k <= N; k++) {
        CHECK_EQ(idmap_get(&m, k), present[k] ? (void *) (uintptr_t) k : NULL);
        count += (size_t) present[k];
    }
    CHECK_EQ(m.count, count);
    idmap_destroy(&m);
}

/* ---- matching rules --------------------------------------------------- */

static void test_resting_orders_set_best_prices(void) {
    mb_book *b = fresh_book();
    CHECK_EQ(mb_limit(b, 1, MB_BUY, 99, 10), MB_OK);
    CHECK_EQ(mb_limit(b, 2, MB_BUY, 100, 5), MB_OK);
    CHECK_EQ(mb_limit(b, 3, MB_SELL, 102, 7), MB_OK);
    CHECK_EQ(mb_best_bid(b), 100);
    CHECK_EQ(mb_best_ask(b), 102);
    CHECK_EQ(mb_depth(b, MB_BUY, 99), 10u);
    CHECK_EQ(ntrades, 0);
    mb_book_destroy(b);
}

static void test_price_time_priority(void) {
    mb_book *b = fresh_book();
    mb_limit(b, 1, MB_SELL, 101, 5); /* older at 101 */
    mb_limit(b, 2, MB_SELL, 100, 5); /* better price */
    mb_limit(b, 3, MB_SELL, 101, 5); /* newer at 101 */
    mb_limit(b, 10, MB_BUY, 101, 12);
    CHECK_EQ(ntrades, 3);
    CHECK_EQ(trades[0].maker_id, 2u); CHECK_EQ(trades[0].price, 100); CHECK_EQ(trades[0].qty, 5u);
    CHECK_EQ(trades[1].maker_id, 1u); CHECK_EQ(trades[1].price, 101); CHECK_EQ(trades[1].qty, 5u);
    CHECK_EQ(trades[2].maker_id, 3u); CHECK_EQ(trades[2].qty, 2u);
    CHECK_EQ(mb_depth(b, MB_SELL, 101), 3u); /* order 3 partially filled, keeps its place */
    CHECK_EQ(mb_best_bid(b), 0);             /* taker fully filled, nothing rests */
    mb_book_destroy(b);
}

static void test_trades_at_maker_price_and_remainder_rests(void) {
    mb_book *b = fresh_book();
    mb_limit(b, 1, MB_BUY, 105, 4);
    mb_limit(b, 2, MB_SELL, 100, 10); /* crosses at 105, rests 6 at 100 */
    CHECK_EQ(ntrades, 1);
    CHECK_EQ(trades[0].price, 105);
    CHECK_EQ(trades[0].taker_side, MB_SELL);
    CHECK_EQ(mb_best_ask(b), 100);
    CHECK_EQ(mb_depth(b, MB_SELL, 100), 6u);
    CHECK_EQ(mb_best_bid(b), 0);
    mb_book_destroy(b);
}

static void test_limit_does_not_trade_through_its_price(void) {
    mb_book *b = fresh_book();
    mb_limit(b, 1, MB_SELL, 100, 5);
    mb_limit(b, 2, MB_SELL, 103, 5);
    mb_limit(b, 3, MB_BUY, 101, 20);
    CHECK_EQ(ntrades, 1);
    CHECK_EQ(mb_best_bid(b), 101);
    CHECK_EQ(mb_depth(b, MB_BUY, 101), 15u);
    CHECK_EQ(mb_best_ask(b), 103);
    mb_book_destroy(b);
}

static void test_market_sweeps_levels_and_discards_rest(void) {
    mb_book *b = fresh_book();
    mb_limit(b, 1, MB_BUY, 100, 3);
    mb_limit(b, 2, MB_BUY, 98, 3);
    uint32_t filled = 0;
    CHECK_EQ(mb_market(b, 9, MB_SELL, 10, &filled), MB_OK);
    CHECK_EQ(filled, 6u);
    CHECK_EQ(ntrades, 2);
    CHECK_EQ(mb_best_bid(b), 0);
    CHECK_EQ(mb_best_ask(b), 0);
    CHECK_EQ(mb_order_count(b), 0u);
    mb_book_destroy(b);
}

static void test_market_rejects_resting_id(void) {
    mb_book *b = fresh_book();
    CHECK_EQ(mb_limit(b, 1, MB_SELL, 100, 5), MB_OK);
    uint32_t filled = 99;
    CHECK_EQ(mb_market(b, 1, MB_BUY, 3, &filled), MB_REJECT_DUP_ID);
    CHECK_EQ(filled, 99u); /* untouched: nothing traded */
    CHECK_EQ(ntrades, 0);
    CHECK_EQ(mb_depth(b, MB_SELL, 100), 5u);
    mb_book_destroy(b);
}

static void test_cancel_updates_best_and_frees_id(void) {
    mb_book *b = fresh_book();
    mb_limit(b, 1, MB_BUY, 100, 5);
    mb_limit(b, 2, MB_BUY, 97, 5);
    CHECK_EQ(mb_cancel(b, 1), MB_OK);
    CHECK_EQ(mb_best_bid(b), 97);
    CHECK_EQ(mb_cancel(b, 1), MB_REJECT_UNKNOWN_ID);
    CHECK_EQ(mb_limit(b, 1, MB_SELL, 200, 1), MB_OK); /* id reusable once gone */
    CHECK_EQ(mb_order_count(b), 2u);
    mb_book_destroy(b);
}

static void test_filled_orders_cannot_be_cancelled(void) {
    mb_book *b = fresh_book();
    mb_limit(b, 1, MB_SELL, 100, 5);
    mb_limit(b, 2, MB_BUY, 100, 5);
    CHECK_EQ(mb_cancel(b, 1), MB_REJECT_UNKNOWN_ID);
    CHECK_EQ(mb_order_count(b), 0u);
    mb_book_destroy(b);
}

static void test_rejects(void) {
    mb_book *b = fresh_book();
    CHECK_EQ(mb_limit(b, 0, MB_BUY, 100, 1), MB_REJECT_BAD_ID);
    CHECK_EQ(mb_limit(b, 1, MB_BUY, 0, 1), MB_REJECT_BAD_PRICE);
    CHECK_EQ(mb_limit(b, 1, MB_BUY, 1001, 1), MB_REJECT_BAD_PRICE);
    CHECK_EQ(mb_limit(b, 1, MB_BUY, 100, 0), MB_REJECT_BAD_QTY);
    CHECK_EQ(mb_limit(b, 1, MB_BUY, 100, 1), MB_OK);
    CHECK_EQ(mb_limit(b, 1, MB_BUY, 99, 1), MB_REJECT_DUP_ID);
    CHECK_EQ(mb_market(b, 5, MB_BUY, 0, NULL), MB_REJECT_BAD_QTY);
    mb_book_destroy(b);
}

static void test_pool_full_rejects_only_the_remainder(void) {
    ntrades = 0;
    mb_book *b = mb_book_create(1000, 2, capture, NULL);
    CHECK_EQ(mb_limit(b, 1, MB_SELL, 100, 1), MB_OK);
    CHECK_EQ(mb_limit(b, 2, MB_SELL, 101, 1), MB_OK);
    CHECK_EQ(mb_limit(b, 3, MB_SELL, 102, 1), MB_REJECT_FULL);
    /* Crossing order frees a slot by filling order 1, then can rest. */
    CHECK_EQ(mb_limit(b, 4, MB_BUY, 100, 3), MB_OK);
    CHECK_EQ(ntrades, 1);
    CHECK_EQ(mb_depth(b, MB_BUY, 100), 2u);
    mb_book_destroy(b);
}

static void test_price_range_edges(void) {
    mb_book *b = fresh_book();
    CHECK_EQ(mb_limit(b, 1, MB_BUY, 1, 1), MB_OK);
    CHECK_EQ(mb_limit(b, 2, MB_SELL, 1000, 1), MB_OK);
    CHECK_EQ(mb_cancel(b, 1), MB_OK);
    CHECK_EQ(mb_cancel(b, 2), MB_OK);
    CHECK_EQ(mb_best_bid(b), 0);
    CHECK_EQ(mb_best_ask(b), 0);
    mb_book_destroy(b);
}

/* ---- journal ---------------------------------------------------------- */

static void test_crc32_known_vector(void) {
    CHECK_EQ(crc32("123456789", 9), 0xCBF43926u);
}

static void test_record_roundtrip_and_corruption(void) {
    jr_record in = {JR_LIMIT, MB_SELL, 123456789012ULL, -5, 77}, out;
    unsigned char buf[JR_RECORD_SIZE];
    jr_encode(&in, buf);
    CHECK_EQ(jr_decode(buf, &out), 0);
    CHECK(out.kind == in.kind && out.side == in.side && out.id == in.id && out.price == in.price && out.qty == in.qty);
    buf[10] ^= 0x01; /* flip one bit */
    CHECK_EQ(jr_decode(buf, &out), -1);
}

static size_t replay_count;
static void count_replay(void *ctx, const jr_record *r) {
    (void) ctx;
    (void) r;
    replay_count++;
}

static void test_recovery_truncates_torn_tail(void) {
    char path[] = "/tmp/mb_journal_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);

    journal j;
    CHECK_EQ(journal_open(&j, path, 1, NULL, NULL, NULL, NULL), 0);
    for (uint64_t i = 1; i <= 5; i++) {
        jr_record r = {JR_LIMIT, MB_BUY, i, 100, 1};
        CHECK_EQ(journal_append(&j, &r), 0);
    }
    /* Simulate a crash halfway through writing record 6. */
    unsigned char partial[JR_RECORD_SIZE];
    jr_record r6 = {JR_LIMIT, MB_BUY, 6, 100, 1};
    jr_encode(&r6, partial);
    fwrite(partial, 1, 13, j.f);
    fclose(j.f);

    size_t replayed = 0, truncated = 0;
    replay_count = 0;
    CHECK_EQ(journal_open(&j, path, 1, count_replay, NULL, &replayed, &truncated), 0);
    CHECK_EQ(replayed, 5u);
    CHECK_EQ(replay_count, 5u);
    CHECK_EQ(truncated, 13u);

    /* New appends land right after the last good record. */
    CHECK_EQ(journal_append(&j, &r6), 0);
    CHECK_EQ(journal_close(&j), 0);
    CHECK_EQ(journal_open(&j, path, 1, NULL, NULL, &replayed, &truncated), 0);
    CHECK_EQ(replayed, 6u);
    CHECK_EQ(truncated, 0u);
    journal_close(&j);
    unlink(path);
}

static void test_recovery_stops_at_corrupt_record(void) {
    char path[] = "/tmp/mb_journal_XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    journal j;
    journal_open(&j, path, 1, NULL, NULL, NULL, NULL);
    for (uint64_t i = 1; i <= 4; i++) {
        jr_record r = {JR_CANCEL, 0, i, 0, 0};
        journal_append(&j, &r);
    }
    journal_close(&j);

    /* Garble a byte inside record 3: records 3 and 4 must be dropped. */
    FILE *f = fopen(path, "r+b");
    fseek(f, 2 * JR_RECORD_SIZE + 9, SEEK_SET);
    fputc(0xFF, f);
    fclose(f);

    size_t replayed = 0, truncated = 0;
    CHECK_EQ(journal_open(&j, path, 1, NULL, NULL, &replayed, &truncated), 0);
    CHECK_EQ(replayed, 2u);
    CHECK_EQ(truncated, 2u * JR_RECORD_SIZE);
    journal_close(&j);
    unlink(path);
}

int main(void) {
    RUN(test_list_fifo_and_remove);
    RUN(test_pool_exhaustion_and_reuse);
    RUN(test_idmap_matches_naive_model);
    RUN(test_resting_orders_set_best_prices);
    RUN(test_price_time_priority);
    RUN(test_trades_at_maker_price_and_remainder_rests);
    RUN(test_limit_does_not_trade_through_its_price);
    RUN(test_market_sweeps_levels_and_discards_rest);
    RUN(test_market_rejects_resting_id);
    RUN(test_cancel_updates_best_and_frees_id);
    RUN(test_filled_orders_cannot_be_cancelled);
    RUN(test_rejects);
    RUN(test_pool_full_rejects_only_the_remainder);
    RUN(test_price_range_edges);
    RUN(test_crc32_known_vector);
    RUN(test_record_roundtrip_and_corruption);
    RUN(test_recovery_truncates_torn_tail);
    RUN(test_recovery_stops_at_corrupt_record);

    printf("%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
