/*
 * matchbook: reads orders from stdin, one per line, and writes fills and
 * rejects to stdout. Output is deterministic, which is what lets
 * tools/fuzz.py diff it against the Python reference model.
 *
 * Input:
 *   L <id> <B|S> <price> <qty>   limit order
 *   M <id> <B|S> <qty>           market order
 *   C <id>                       cancel
 *   X <id> <qty>                 reduce a resting order by qty, keeping its priority
 *   P                            print top of book
 *   # ...                        comment
 *
 * Output:
 *   T <maker_id> <taker_id> <price> <qty>
 *   R <id> <reason>
 *   BOOK <bid> <bid_qty> <ask> <ask_qty>   (0 when a side is empty)
 *
 * With -j <file>, every order is written to a checksummed write-ahead journal
 * before it is applied. On the next start the journal is replayed (silently)
 * to rebuild the book, so a crash loses at most the unsynced tail.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "book.h"
#include "journal.h"

static int replaying = 0;

static void print_trade(void *ctx, const mb_trade *t) {
    (void) ctx;
    if (replaying) {
        return; /* these fills were already reported before the restart */
    }
    printf("T %" PRIu64 " %" PRIu64 " %" PRId64 " %" PRIu32 "\n", t->maker_id, t->taker_id, t->price, t->qty);
}

static int parse_side(const char *s, mb_side *out) {
    if (strcmp(s, "B") == 0) { *out = MB_BUY; return 0; }
    if (strcmp(s, "S") == 0) { *out = MB_SELL; return 0; }
    return -1;
}

static mb_status apply(mb_book *book, const jr_record *r) {
    switch (r->kind) {
        case JR_LIMIT: return mb_limit(book, r->id, (mb_side) r->side, r->price, r->qty);
        case JR_MARKET: return mb_market(book, r->id, (mb_side) r->side, r->qty, NULL);
        case JR_REDUCE: return mb_reduce(book, r->id, r->qty);
        default: return mb_cancel(book, r->id);
    }
}

static void replay_record(void *ctx, const jr_record *r) {
    apply((mb_book *) ctx, r);
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [-p max_price] [-n max_orders] [-j journal] [--fsync-every N] < orders.txt\n", prog);
}

int main(int argc, char **argv) {
    int64_t max_price = 100000;
    size_t max_orders = 1000000;
    const char *journal_path = NULL;
    unsigned fsync_every = 64;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            max_price = strtoll(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_orders = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
            journal_path = argv[++i];
        } else if (strcmp(argv[i], "--fsync-every") == 0 && i + 1 < argc) {
            fsync_every = (unsigned) strtoul(argv[++i], NULL, 10);
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    mb_book *book = mb_book_create(max_price, max_orders, print_trade, NULL);
    if (book == NULL) {
        fprintf(stderr, "failed to create book\n");
        return 1;
    }
    journal jnl = {0};
    if (journal_path != NULL) {
        size_t replayed = 0, truncated = 0;
        replaying = 1;
        if (journal_open(&jnl, journal_path, fsync_every, replay_record, book, &replayed, &truncated) != 0) {
            perror(journal_path);
            mb_book_destroy(book);
            return 1;
        }
        replaying = 0;
        fprintf(stderr, "journal: replayed %zu orders, discarded %zu bytes of torn tail\n", replayed, truncated);
    }

    static char outbuf[1 << 16];
    setvbuf(stdout, outbuf, _IOFBF, sizeof outbuf);

    char line[256];
    unsigned long lineno = 0;
    while (fgets(line, sizeof line, stdin) != NULL) {
        lineno++;
        char cmd[2], side_s[2];
        uint64_t id;
        int64_t price;
        uint32_t qty;
        mb_side side;
        mb_status st;

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
            continue;
        }
        if (sscanf(line, "%1s", cmd) != 1) {
            continue;
        }
        jr_record rec = {0};
        switch (cmd[0]) {
            case 'L':
                if (sscanf(line, "L %" SCNu64 " %1s %" SCNd64 " %" SCNu32, &id, side_s, &price, &qty) != 4 ||
                    parse_side(side_s, &side) != 0) {
                    goto malformed;
                }
                rec = (jr_record) {JR_LIMIT, (uint8_t) side, id, price, qty};
                break;
            case 'M':
                if (sscanf(line, "M %" SCNu64 " %1s %" SCNu32, &id, side_s, &qty) != 3 ||
                    parse_side(side_s, &side) != 0) {
                    goto malformed;
                }
                rec = (jr_record) {JR_MARKET, (uint8_t) side, id, 0, qty};
                break;
            case 'C':
                if (sscanf(line, "C %" SCNu64, &id) != 1) {
                    goto malformed;
                }
                rec = (jr_record) {JR_CANCEL, 0, id, 0, 0};
                break;
            case 'X':
                if (sscanf(line, "X %" SCNu64 " %" SCNu32, &id, &qty) != 2) {
                    goto malformed;
                }
                rec = (jr_record) {JR_REDUCE, 0, id, 0, qty};
                break;
            case 'P': {
                int64_t bid = mb_best_bid(book), ask = mb_best_ask(book);
                printf("BOOK %" PRId64 " %" PRIu64 " %" PRId64 " %" PRIu64 "\n",
                       bid, mb_depth(book, MB_BUY, bid), ask, mb_depth(book, MB_SELL, ask));
                continue;
            }
            default:
                goto malformed;
        }
        /* Write-ahead: the order is durable (up to group commit) before it can trade. */
        if (journal_path != NULL && journal_append(&jnl, &rec) != 0) {
            perror("journal append");
            journal_close(&jnl);
            mb_book_destroy(book);
            return 1;
        }
        st = apply(book, &rec);
        if (st != MB_OK) {
            printf("R %" PRIu64 " %s\n", id, mb_status_str(st));
        }
        continue;
    malformed:
        fprintf(stderr, "line %lu: malformed input: %s", lineno, line);
    }

    int rc = 0;
    if (journal_path != NULL && journal_close(&jnl) != 0) {
        perror("journal close");
        rc = 1;
    }
    mb_book_destroy(book);
    return rc;
}
