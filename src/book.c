#include "book.h"

#include <stdlib.h>

#include "idmap.h"
#include "list.h"
#include "pool.h"

typedef struct order {
    list_elem elem; /* position in its price level's FIFO */
    uint64_t id;
    int64_t price;
    uint32_t qty; /* remaining */
    mb_side side;
} order;

typedef struct level {
    list fifo;
    uint64_t total_qty;
} level;

struct mb_book {
    int64_t max_price;
    level *levels[2]; /* indexed by side, then by price; index 0 unused */
    int64_t best[2];  /* best bid / best ask, 0 when that side is empty */
    pool orders;
    idmap by_id;
    mb_trade_fn on_trade;
    void *ctx;
};

/* ---- helpers ---------------------------------------------------------- */

static inline mb_side opposite(mb_side s) {
    return s == MB_BUY ? MB_SELL : MB_BUY;
}

/* Would a taker on `side` with limit `limit` trade at `price`? */
static inline int crosses(mb_side side, int64_t limit, int64_t price) {
    return side == MB_BUY ? price <= limit : price >= limit;
}

/* Is `a` a better price than `b` for resting orders on `side`? */
static inline int better(mb_side side, int64_t a, int64_t b) {
    return side == MB_BUY ? a > b : a < b;
}

/*
 * After the best level on `side` empties, walk away from the spread to the
 * next non-empty level. Worst case is O(price range), but in practice the
 * next level is a few ticks away.
 */
static void advance_best(mb_book *b, mb_side side) {
    level *lv = b->levels[side];
    int64_t p = b->best[side];
    if (side == MB_BUY) {
        for (p = p - 1; p >= 1; p--) {
            if (lv[p].total_qty > 0) {
                b->best[side] = p;
                return;
            }
        }
    } else {
        for (p = p + 1; p <= b->max_price; p++) {
            if (lv[p].total_qty > 0) {
                b->best[side] = p;
                return;
            }
        }
    }
    b->best[side] = 0;
}

static void release(mb_book *b, order *o) {
    idmap_remove(&b->by_id, o->id);
    pool_free(&b->orders, o);
}

/*
 * Fills up to `qty` of an incoming order against the opposite side, best price
 * first and oldest order first within a price. Returns the quantity filled.
 * `limit` is ignored for market orders (pass 0).
 */
static uint32_t match(mb_book *b, uint64_t taker_id, mb_side side, int64_t limit, int is_market, uint32_t qty) {
    mb_side maker_side = opposite(side);
    level *levels = b->levels[maker_side];
    uint32_t filled = 0;

    while (qty > 0) {
        int64_t price = b->best[maker_side];
        if (price == 0 || (!is_market && !crosses(side, limit, price))) {
            break;
        }
        level *lv = &levels[price];
        while (qty > 0 && !list_empty(&lv->fifo)) {
            order *maker = list_entry(list_front(&lv->fifo), order, elem);
            uint32_t fill = maker->qty < qty ? maker->qty : qty;

            maker->qty -= fill;
            lv->total_qty -= fill;
            qty -= fill;
            filled += fill;

            if (b->on_trade != NULL) {
                mb_trade t = {maker->id, taker_id, price, fill, side};
                b->on_trade(b->ctx, &t);
            }
            if (maker->qty == 0) {
                list_remove(&maker->elem);
                release(b, maker);
            }
        }
        if (lv->total_qty == 0) {
            advance_best(b, maker_side);
        }
    }
    return filled;
}

/* ---- public API ------------------------------------------------------- */

mb_book *mb_book_create(int64_t max_price, size_t max_orders, mb_trade_fn on_trade, void *ctx) {
    if (max_price < 1 || max_orders < 1) {
        return NULL;
    }
    mb_book *b = calloc(1, sizeof *b);
    if (b == NULL) {
        return NULL;
    }
    b->max_price = max_price;
    b->on_trade = on_trade;
    b->ctx = ctx;

    for (int s = 0; s < 2; s++) {
        b->levels[s] = malloc((size_t) (max_price + 1) * sizeof(level));
        if (b->levels[s] == NULL) {
            mb_book_destroy(b);
            return NULL;
        }
        for (int64_t p = 0; p <= max_price; p++) {
            list_init(&b->levels[s][p].fifo);
            b->levels[s][p].total_qty = 0;
        }
    }
    if (pool_init(&b->orders, sizeof(order), max_orders) != 0 || idmap_init(&b->by_id, max_orders) != 0) {
        mb_book_destroy(b);
        return NULL;
    }
    return b;
}

void mb_book_destroy(mb_book *b) {
    if (b == NULL) {
        return;
    }
    free(b->levels[MB_BUY]);
    free(b->levels[MB_SELL]);
    pool_destroy(&b->orders);
    idmap_destroy(&b->by_id);
    free(b);
}

mb_status mb_limit(mb_book *b, uint64_t id, mb_side side, int64_t price, uint32_t qty) {
    if (id == 0) return MB_REJECT_BAD_ID;
    if (qty == 0) return MB_REJECT_BAD_QTY;
    if (price < 1 || price > b->max_price) return MB_REJECT_BAD_PRICE;
    if (idmap_get(&b->by_id, id) != NULL) return MB_REJECT_DUP_ID;

    qty -= match(b, id, side, price, 0, qty);
    if (qty == 0) {
        return MB_OK;
    }

    order *o = pool_alloc(&b->orders);
    if (o == NULL) {
        /* The matched part already traded; only the remainder is rejected. */
        return MB_REJECT_FULL;
    }
    o->id = id;
    o->price = price;
    o->qty = qty;
    o->side = side;
    idmap_put(&b->by_id, id, o);

    level *lv = &b->levels[side][price];
    list_push_back(&lv->fifo, &o->elem);
    lv->total_qty += qty;
    if (b->best[side] == 0 || better(side, price, b->best[side])) {
        b->best[side] = price;
    }
    return MB_OK;
}

mb_status mb_market(mb_book *b, uint64_t id, mb_side side, uint32_t qty, uint32_t *filled) {
    if (id == 0) return MB_REJECT_BAD_ID;
    if (qty == 0) return MB_REJECT_BAD_QTY;
    /* Same rule as limit orders: a resting order's id can't be reused, or the
     * market order could trade against itself. */
    if (idmap_get(&b->by_id, id) != NULL) return MB_REJECT_DUP_ID;
    uint32_t f = match(b, id, side, 0, 1, qty);
    if (filled != NULL) {
        *filled = f;
    }
    return MB_OK;
}

mb_status mb_cancel(mb_book *b, uint64_t id) {
    if (id == 0) return MB_REJECT_BAD_ID;
    order *o = idmap_get(&b->by_id, id);
    if (o == NULL) {
        return MB_REJECT_UNKNOWN_ID;
    }
    level *lv = &b->levels[o->side][o->price];
    list_remove(&o->elem);
    lv->total_qty -= o->qty;
    if (lv->total_qty == 0 && b->best[o->side] == o->price) {
        advance_best(b, o->side);
    }
    release(b, o);
    return MB_OK;
}

int64_t mb_best_bid(const mb_book *b) { return b->best[MB_BUY]; }
int64_t mb_best_ask(const mb_book *b) { return b->best[MB_SELL]; }

uint64_t mb_depth(const mb_book *b, mb_side side, int64_t price) {
    if (price < 1 || price > b->max_price) {
        return 0;
    }
    return b->levels[side][price].total_qty;
}

size_t mb_order_count(const mb_book *b) {
    return b->orders.in_use;
}

const char *mb_status_str(mb_status s) {
    switch (s) {
        case MB_OK: return "OK";
        case MB_REJECT_BAD_ID: return "BAD_ID";
        case MB_REJECT_DUP_ID: return "DUP_ID";
        case MB_REJECT_BAD_PRICE: return "BAD_PRICE";
        case MB_REJECT_BAD_QTY: return "BAD_QTY";
        case MB_REJECT_FULL: return "FULL";
        case MB_REJECT_UNKNOWN_ID: return "UNKNOWN_ID";
    }
    return "?";
}
