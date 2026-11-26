/*
 * Write-ahead journal for crash recovery.
 *
 * Every accepted command is appended to the journal *before* it is applied
 * to the book. On restart, replaying the journal rebuilds the exact same book,
 * because matching is deterministic.
 *
 * Each record is fixed-size and ends with a CRC-32 over its contents. A crash
 * mid-write can leave a torn (partial or garbled) record at the tail; recovery
 * stops at the first record that is short or fails its checksum, truncates the
 * file there, and resumes appending after the last good record. This is the
 * same idea as a journaling file system's transaction checksum.
 *
 * Durability is a tradeoff: fsync after every record is safest but slow, so
 * fsync_every batches N records per fsync (group commit). Up to N-1
 * acknowledged records can be lost on power failure.
 */
#ifndef MB_JOURNAL_H
#define MB_JOURNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { JR_LIMIT = 1, JR_MARKET = 2, JR_CANCEL = 3, JR_REDUCE = 4 } jr_kind;

typedef struct jr_record {
    uint8_t kind; /* jr_kind */
    uint8_t side; /* mb_side */
    uint64_t id;
    int64_t price;
    uint32_t qty;
} jr_record;

#define JR_RECORD_SIZE 32 /* bytes on disk, including the checksum */

typedef struct journal {
    FILE *f;
    int fd;
    unsigned fsync_every;
    unsigned unsynced;
} journal;

typedef void (*jr_replay_fn)(void *ctx, const jr_record *rec);

/*
 * Opens (creating if needed) the journal at `path`, replays every valid record
 * through `replay`, truncates any torn tail, and positions for appending.
 * Returns 0 on success. *replayed and *truncated_bytes may be NULL.
 */
int journal_open(journal *j, const char *path, unsigned fsync_every,
                 jr_replay_fn replay, void *ctx, size_t *replayed, size_t *truncated_bytes);

int journal_append(journal *j, const jr_record *rec);
int journal_sync(journal *j);
int journal_close(journal *j);

/* Exposed for tests. */
uint32_t crc32(const void *data, size_t len);
void jr_encode(const jr_record *rec, unsigned char out[JR_RECORD_SIZE]);
/* Returns 0 if the checksum matches, -1 otherwise. */
int jr_decode(const unsigned char in[JR_RECORD_SIZE], jr_record *rec);

#ifdef __cplusplus
}
#endif

#endif
