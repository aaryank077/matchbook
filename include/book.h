/*
 * Limit order book with price-time priority matching.
 *
 * Prices are integer ticks in [1, max_price] (no floating point anywhere), and
 * each side keeps a directly indexed array of price levels, so finding the
 * level for a price is a single array access. Each level is a FIFO of resting
 * orders (intrusive list), so the oldest order at the best price always fills
 * first. Orders come from a preallocated pool and are indexed by id in a hash
 * map, which makes cancels O(1).
 */
#ifndef MB_BOOK_H
#define MB_BOOK_H

#include <stddef.h>
#include <stdint.h>

typedef enum { MB_BUY = 0, MB_SELL = 1 } mb_side;

typedef enum {
    MB_OK = 0,
    MB_REJECT_BAD_ID,    /* id 0 is reserved */
    MB_REJECT_DUP_ID,    /* an order with this id is already resting */
    MB_REJECT_BAD_PRICE, /* outside [1, max_price] */
    MB_REJECT_BAD_QTY,   /* quantity must be positive */
    MB_REJECT_FULL,      /* order pool exhausted */
    MB_REJECT_UNKNOWN_ID /* cancel for an order that is not resting */
} mb_status;

typedef struct mb_trade {
    uint64_t maker_id; /* resting order */
    uint64_t taker_id; /* incoming order */
    int64_t price;     /* always the maker's price */
    uint32_t qty;
    mb_side taker_side;
} mb_trade;

typedef void (*mb_trade_fn)(void *ctx, const mb_trade *trade);

typedef struct mb_book mb_book;

/* Returns NULL on allocation failure or invalid arguments. on_trade may be NULL. */
mb_book *mb_book_create(int64_t max_price, size_t max_orders, mb_trade_fn on_trade, void *ctx);
void mb_book_destroy(mb_book *book);

/* Matches against the opposite side up to `price`, then rests any remainder. */
mb_status mb_limit(mb_book *book, uint64_t id, mb_side side, int64_t price, uint32_t qty);

/* Matches at any price; the unfilled remainder is discarded. *filled may be NULL. */
mb_status mb_market(mb_book *book, uint64_t id, mb_side side, uint32_t qty, uint32_t *filled);

mb_status mb_cancel(mb_book *book, uint64_t id);

/* Best prices, or 0 if that side is empty. */
int64_t mb_best_bid(const mb_book *book);
int64_t mb_best_ask(const mb_book *book);

/* Total resting quantity at a price on one side (0 if none or out of range). */
uint64_t mb_depth(const mb_book *book, mb_side side, int64_t price);

size_t mb_order_count(const mb_book *book);

const char *mb_status_str(mb_status status);

#endif
