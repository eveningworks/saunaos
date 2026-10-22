#ifndef SCSI_H
#define SCSI_H

// SCSI command blocks and the answers to them, for the transports that
// carry SCSI: USB bulk-only (usb_storage.c) and UAS. Every one of Linux,
// Windows and macOS puts SCSI between a USB transport and the disk; this
// is that layer at the size toy-os needs -- the CDBs a direct-access disk
// takes, their replies parsed, and sense data decoded -- with no hosts,
// targets or queues. A transport runs a CDB; this builds and reads it.
//
// Everything on the wire is BIG-ENDIAN; the builders and parsers here are
// the only code that knows.
#include <stdint.h>

#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_START_STOP_UNIT  0x1B
#define SCSI_PREVENT_ALLOW    0x1E
#define SCSI_READ_CAPACITY10  0x25
#define SCSI_READ10           0x28
#define SCSI_WRITE10          0x2A
#define SCSI_SYNC_CACHE10     0x35
#define SCSI_READ16           0x88
#define SCSI_WRITE16          0x8A
#define SCSI_SERVICE_IN16     0x9E   // READ CAPACITY(16) is its action 0x10

#define SCSI_INQUIRY_LEN      36
#define SCSI_SENSE_LEN        18

// Each builder fills `cdb` and returns its length (6, 10 or 16).
int scsi_cdb_test_unit_ready(uint8_t *cdb);
int scsi_cdb_request_sense(uint8_t *cdb, uint8_t alloc);
int scsi_cdb_inquiry(uint8_t *cdb, uint8_t alloc);
int scsi_cdb_read_capacity10(uint8_t *cdb);
int scsi_cdb_read_capacity16(uint8_t *cdb, uint8_t alloc);
int scsi_cdb_sync_cache(uint8_t *cdb);
// `start` 0 + `eject` 1 is "stop and eject the medium" -- Eject's last word.
int scsi_cdb_start_stop(uint8_t *cdb, int start, int eject);
// READ/WRITE of `blocks` logical blocks at `lba`: (10) when both fit,
// (16) otherwise. 0 for a request neither form can carry.
int scsi_cdb_rw(uint8_t *cdb, int write, uint64_t lba, uint32_t blocks);

struct scsi_inquiry {
    uint8_t type;        // peripheral device type: 0 = a direct-access disk
    uint8_t removable;   // RMB: the MEDIUM is removable (a card reader's slot)
    char vendor[9];      // trailing spaces trimmed
    char product[17];
};
// 0 for a reply too short to hold the fields.
int scsi_parse_inquiry(const uint8_t *buf, uint32_t len, struct scsi_inquiry *out);

// READ CAPACITY: the LAST block's address, and the block length. (10)
// answers 0xFFFFFFFF for a disk past 2 TiB, which means "ask (16)".
int scsi_parse_capacity10(const uint8_t *buf, uint32_t len, uint64_t *last_lba, uint32_t *block);
int scsi_parse_capacity16(const uint8_t *buf, uint32_t len, uint64_t *last_lba, uint32_t *block);

#define SCSI_KEY_NO_SENSE        0x0
#define SCSI_KEY_RECOVERED       0x1
#define SCSI_KEY_NOT_READY       0x2
#define SCSI_KEY_MEDIUM_ERROR    0x3
#define SCSI_KEY_HARDWARE_ERROR  0x4
#define SCSI_KEY_ILLEGAL_REQUEST 0x5
#define SCSI_KEY_UNIT_ATTENTION  0x6
#define SCSI_KEY_DATA_PROTECT    0x7

struct scsi_sense {
    uint8_t key, asc, ascq;
};
// Fixed (0x70/0x71) and descriptor (0x72/0x73) formats. 0 for anything else.
int scsi_parse_sense(const uint8_t *buf, uint32_t len, struct scsi_sense *out);
// "not ready", "unit attention", ... Never NULL.
const char *scsi_key_name(uint8_t key);
// NOT READY with ASC 0x3A: no medium -- an empty card slot, not a fault.
int scsi_sense_no_medium(const struct scsi_sense *s);

#endif
