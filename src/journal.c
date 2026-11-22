#include "journal.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

/* CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320), table built on first use. */
uint32_t crc32(const void *data, size_t len) {
    static uint32_t table[256];
    static int ready = 0;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
        ready = 1;
    }
    const unsigned char *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

/* Little-endian, explicit layout so the file format does not depend on struct padding. */
static void put_u32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (unsigned char) (v >> (8 * i));
}
static void put_u64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char) (v >> (8 * i));
}
static uint32_t get_u32(const unsigned char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t) p[i] << (8 * i);
    return v;
}
static uint64_t get_u64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t) p[i] << (8 * i);
    return v;
}

/*
 * Layout (32 bytes):
 *   0  kind (1)   1  side (1)   2  reserved (2)
 *   4  qty (4)    8  id (8)     16 price (8)
 *   24 reserved (4)             28 crc32 of bytes 0..27 (4)
 */
void jr_encode(const jr_record *rec, unsigned char out[JR_RECORD_SIZE]) {
    memset(out, 0, JR_RECORD_SIZE);
    out[0] = rec->kind;
    out[1] = rec->side;
    put_u32(out + 4, rec->qty);
    put_u64(out + 8, rec->id);
    put_u64(out + 16, (uint64_t) rec->price);
    put_u32(out + 28, crc32(out, 28));
}

int jr_decode(const unsigned char in[JR_RECORD_SIZE], jr_record *rec) {
    if (get_u32(in + 28) != crc32(in, 28)) {
        return -1;
    }
    if (in[0] < JR_LIMIT || in[0] > JR_CANCEL) {
        return -1;
    }
    rec->kind = in[0];
    rec->side = in[1];
    rec->qty = get_u32(in + 4);
    rec->id = get_u64(in + 8);
    rec->price = (int64_t) get_u64(in + 16);
    return 0;
}

int journal_open(journal *j, const char *path, unsigned fsync_every,
                 jr_replay_fn replay, void *ctx, size_t *replayed, size_t *truncated_bytes) {
    j->f = fopen(path, "a+b");
    if (j->f == NULL) {
        return -1;
    }
    j->fd = fileno(j->f);
    j->fsync_every = fsync_every ? fsync_every : 1;
    j->unsynced = 0;

    if (fseeko(j->f, 0, SEEK_END) != 0) goto fail;
    off_t size = ftello(j->f);
    rewind(j->f);

    unsigned char buf[JR_RECORD_SIZE];
    size_t good = 0;
    for (;;) {
        size_t got = fread(buf, 1, JR_RECORD_SIZE, j->f);
        jr_record rec;
        if (got < JR_RECORD_SIZE || jr_decode(buf, &rec) != 0) {
            break; /* clean end, torn tail, or corruption: stop here */
        }
        if (replay != NULL) {
            replay(ctx, &rec);
        }
        good++;
    }

    off_t good_bytes = (off_t) (good * JR_RECORD_SIZE);
    if (good_bytes < size) {
        if (ftruncate(j->fd, good_bytes) != 0 || fsync(j->fd) != 0) goto fail;
    }
    if (fseeko(j->f, 0, SEEK_END) != 0) goto fail;

    if (replayed) *replayed = good;
    if (truncated_bytes) *truncated_bytes = (size_t) (size - good_bytes);
    return 0;

fail: {
        int saved = errno;
        fclose(j->f);
        j->f = NULL;
        errno = saved;
        return -1;
    }
}

int journal_append(journal *j, const jr_record *rec) {
    unsigned char buf[JR_RECORD_SIZE];
    jr_encode(rec, buf);
    if (fwrite(buf, 1, JR_RECORD_SIZE, j->f) != JR_RECORD_SIZE) {
        return -1;
    }
    if (++j->unsynced >= j->fsync_every) {
        return journal_sync(j);
    }
    return 0;
}

int journal_sync(journal *j) {
    if (fflush(j->f) != 0 || fsync(j->fd) != 0) {
        return -1;
    }
    j->unsynced = 0;
    return 0;
}

int journal_close(journal *j) {
    if (j->f == NULL) {
        return 0;
    }
    int rc = journal_sync(j);
    if (fclose(j->f) != 0) rc = -1;
    j->f = NULL;
    return rc;
}
