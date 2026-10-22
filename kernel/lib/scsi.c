// SCSI command blocks and replies -- see kernel/include/kernel/scsi.h.
#include "scsi.h"
#include "string.h"
#include "ktest.h"

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

int scsi_cdb_test_unit_ready(uint8_t *cdb) {
    k_memset(cdb, 0, 6);
    return 6;
}

int scsi_cdb_request_sense(uint8_t *cdb, uint8_t alloc) {
    k_memset(cdb, 0, 6);
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = alloc;
    return 6;
}

int scsi_cdb_inquiry(uint8_t *cdb, uint8_t alloc) {
    k_memset(cdb, 0, 6);
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = alloc;
    return 6;
}

int scsi_cdb_read_capacity10(uint8_t *cdb) {
    k_memset(cdb, 0, 10);
    cdb[0] = SCSI_READ_CAPACITY10;
    return 10;
}

int scsi_cdb_read_capacity16(uint8_t *cdb, uint8_t alloc) {
    k_memset(cdb, 0, 16);
    cdb[0] = SCSI_SERVICE_IN16;
    cdb[1] = 0x10;           // READ CAPACITY(16)
    cdb[13] = alloc;
    return 16;
}

int scsi_cdb_sync_cache(uint8_t *cdb) {
    k_memset(cdb, 0, 10);
    cdb[0] = SCSI_SYNC_CACHE10;   // LBA 0, 0 blocks: the whole cache
    return 10;
}

int scsi_cdb_start_stop(uint8_t *cdb, int start, int eject) {
    k_memset(cdb, 0, 6);
    cdb[0] = SCSI_START_STOP_UNIT;
    cdb[4] = (uint8_t)((start ? 1 : 0) | (eject ? 2 : 0));
    return 6;
}

int scsi_cdb_rw(uint8_t *cdb, int write, uint64_t lba, uint32_t blocks) {
    if (blocks == 0) return 0;
    if (lba + blocks <= 0x100000000ull && blocks <= 0xFFFF) {
        k_memset(cdb, 0, 10);
        cdb[0] = write ? SCSI_WRITE10 : SCSI_READ10;
        put_be32(cdb + 2, (uint32_t)lba);
        cdb[7] = (uint8_t)(blocks >> 8);
        cdb[8] = (uint8_t)blocks;
        return 10;
    }
    k_memset(cdb, 0, 16);
    cdb[0] = write ? SCSI_WRITE16 : SCSI_READ16;
    put_be64(cdb + 2, lba);
    put_be32(cdb + 10, blocks);
    return 16;
}

// Space-padded ASCII into a C string, trailing spaces dropped.
static void copy_trim(char *out, const uint8_t *in, int n) {
    int len = n;
    while (len > 0 && (in[len - 1] == ' ' || in[len - 1] == 0)) len--;
    for (int i = 0; i < len; i++) out[i] = (in[i] >= 0x20 && in[i] < 0x7F) ? (char)in[i] : '?';
    out[len] = 0;
}

int scsi_parse_inquiry(const uint8_t *buf, uint32_t len, struct scsi_inquiry *out) {
    if (len < SCSI_INQUIRY_LEN) return 0;
    out->type = buf[0] & 0x1F;
    out->removable = (buf[1] & 0x80) ? 1 : 0;
    copy_trim(out->vendor, buf + 8, 8);
    copy_trim(out->product, buf + 16, 16);
    return 1;
}

static int block_ok(uint32_t b) { return b == 512 || b == 4096; }

int scsi_parse_capacity10(const uint8_t *buf, uint32_t len, uint64_t *last_lba, uint32_t *block) {
    if (len < 8) return 0;
    *last_lba = be32(buf);
    *block = be32(buf + 4);
    return block_ok(*block);
}

int scsi_parse_capacity16(const uint8_t *buf, uint32_t len, uint64_t *last_lba, uint32_t *block) {
    if (len < 12) return 0;
    *last_lba = be64(buf);
    *block = be32(buf + 8);
    return block_ok(*block);
}

int scsi_parse_sense(const uint8_t *buf, uint32_t len, struct scsi_sense *out) {
    if (len < 4) return 0;
    uint8_t code = buf[0] & 0x7F;
    if (code == 0x70 || code == 0x71) {
        if (len < 14) return 0;
        out->key = buf[2] & 0x0F;
        out->asc = buf[12];
        out->ascq = buf[13];
        return 1;
    }
    if (code == 0x72 || code == 0x73) {
        out->key = buf[1] & 0x0F;
        out->asc = buf[2];
        out->ascq = buf[3];
        return 1;
    }
    return 0;
}

const char *scsi_key_name(uint8_t key) {
    static const char *const names[] = {
        "no sense", "recovered error", "not ready", "medium error", "hardware error",
        "illegal request", "unit attention", "data protect",
    };
    return key < sizeof names / sizeof names[0] ? names[key] : "other";
}

int scsi_sense_no_medium(const struct scsi_sense *s) {
    return s->key == SCSI_KEY_NOT_READY && s->asc == 0x3A;
}

// ---- tests ------------------------------------------------------------

KTEST("scsi", "READ picks (10) while it fits and (16) past it, big-endian") {
    uint8_t c[16];
    KTEST_ASSERT_EQ(scsi_cdb_rw(c, 0, 0x12345678, 8), 10);
    KTEST_ASSERT(c[0] == SCSI_READ10 && c[2] == 0x12 && c[5] == 0x78 && c[7] == 0 && c[8] == 8);
    KTEST_ASSERT_EQ(scsi_cdb_rw(c, 1, 0xFFFFFFF8ull, 8), 10);   // ends exactly at 2^32
    KTEST_ASSERT_EQ(scsi_cdb_rw(c, 1, 0xFFFFFFF9ull, 8), 16);   // one past it
    KTEST_ASSERT(c[0] == SCSI_WRITE16 && c[5] == 0 && c[6] == 0xFF && c[9] == 0xF9 && c[13] == 8);
    KTEST_ASSERT_EQ(scsi_cdb_rw(c, 0, 0, 0x10000), 16);          // too many blocks for (10)
    KTEST_ASSERT_EQ(scsi_cdb_rw(c, 0, 0, 0), 0);
}

KTEST("scsi", "INQUIRY, READ CAPACITY and sense parse, and refuse short replies") {
    uint8_t inq[36] = { 0x00, 0x80 };
    k_memcpy(inq + 8, "Kingston", 8);
    k_memcpy(inq + 16, "DataTraveler 3.0", 16);
    struct scsi_inquiry q;
    KTEST_ASSERT(scsi_parse_inquiry(inq, 36, &q));
    KTEST_ASSERT(q.type == 0 && q.removable == 1);
    KTEST_ASSERT(k_strcmp(q.vendor, "Kingston") == 0 && k_strcmp(q.product, "DataTraveler 3.0") == 0);
    KTEST_ASSERT(!scsi_parse_inquiry(inq, 35, &q));

    uint8_t cap[8] = { 0x01, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00 };
    uint64_t last; uint32_t bs;
    KTEST_ASSERT(scsi_parse_capacity10(cap, 8, &last, &bs));
    KTEST_ASSERT(last == 0x01D00000 && bs == 512);
    cap[6] = 0x08;   // 2048-byte blocks: a CD, refused
    KTEST_ASSERT(!scsi_parse_capacity10(cap, 8, &last, &bs));

    uint8_t sense[18] = { 0x70, 0, SCSI_KEY_NOT_READY };
    sense[12] = 0x3A;
    struct scsi_sense s;
    KTEST_ASSERT(scsi_parse_sense(sense, 18, &s));
    KTEST_ASSERT(scsi_sense_no_medium(&s));
    uint8_t desc[8] = { 0x72, SCSI_KEY_UNIT_ATTENTION, 0x28, 0x00 };
    KTEST_ASSERT(scsi_parse_sense(desc, 8, &s) && s.key == SCSI_KEY_UNIT_ATTENTION && s.asc == 0x28);
    KTEST_ASSERT(!scsi_parse_sense(sense, 13, &s));
}
