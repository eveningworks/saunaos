// Tests for MBR/GPT partition tables (kernel/drivers/partition.c).
//
// TWO KINDS, and the second is the one worth having.
//
// The validator tests are pure logic against the real disk's size --
// they assert the REFUSALS, which is where a table writer does its
// actual work. A writer that encodes correctly and validates loosely
// will happily produce two partitions sharing sectors, and nothing
// downstream would notice until a filesystem in one of them started
// reading the other one's data.
//
// The round-trip test writes a real GPT and reads it back through the
// real parser. That covers, in one assertion set, the CRC32 over both
// the header and the entry array, every field offset in a 92-byte
// header and a 128-byte entry, GPT's mixed-endian GUID layout, the
// INCLUSIVE end LBA, and the UTF-16LE name conversion in both
// directions -- none of which a validator test can see. Before this
// existed, the GPT half was only ever proved by a host-compiled
// harness against a synthetic image (docs/decisions/storage.md, "GPT
// header verification"), because TFS2 overwrote LBA 1 on every boot.
// TFS3 reserves volume blocks 0-7, so that blocker is gone.
//
// IT RUNS ON A RAM DISK, NEVER THE REAL ONE. Writing a partition table
// to the disk this kernel booted from would destroy the filesystem the
// rest of the suite is using. The active block device is swapped for a
// scratch RAM device and PUT BACK, with preemption held off across the
// whole window -- a ring-3 syscall landing in the middle would do its
// filesystem I/O against a 512 KiB RAM disk with no filesystem on it.
#include "ktest.h"
#include "partition.h"
#include "block.h"
#include "heap.h"
#include "scheduler.h"
#include "string.h"
#include "kcrc.h"
#include "errno.h"

// 1024 sectors. Big enough for a GPT's whole geometry -- 34 at the
// front, 33 at the back -- with room between for two partitions.
#define SCRATCH_SECTORS 1024
#define SCRATCH_BYTES (SCRATCH_SECTORS * 512)

// The BIOS boot and EFI System type GUIDs, spelled out here rather than
// shared with partition.c on purpose: a test that imports the constant
// under test asserts that the code equals itself. These bytes came off
// the disk tools/install_grub.py writes.
static const uint8_t TYPE_BIOS_BOOT[16] = {
    0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6F, 0x6E,
    0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49,
};
static const uint8_t TYPE_ESP[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
};
static const uint8_t TYPE_BASIC_DATA[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7,
};

// What stops `fsformat` eating the bootloader. disk.img's first two
// partitions hold GRUB's core.img and /boot/kernel.bin; vfs.c's scan
// asks this about every entry before it probes one or leaves one
// active, so getting it wrong is not a wrong listing -- it is a
// formatted boot partition.
KTEST("partition", "the firmware's partitions are recognised as not ours") {
    struct partition_entry pe;

    k_memset(&pe, 0, sizeof pe);
    k_memcpy(pe.gpt_type_guid, TYPE_BIOS_BOOT, 16);
    KTEST_ASSERT(partition_is_firmware(&pe, PART_TABLE_GPT));

    k_memcpy(pe.gpt_type_guid, TYPE_ESP, 16);
    KTEST_ASSERT(partition_is_firmware(&pe, PART_TABLE_GPT));

    // The one every toy-os filesystem partition carries -- if this came
    // back true the OS would refuse to mount its own root.
    k_memcpy(pe.gpt_type_guid, TYPE_BASIC_DATA, 16);
    KTEST_ASSERT(!partition_is_firmware(&pe, PART_TABLE_GPT));

    // An MBR entry has no GUID at all, so the same table must be read
    // through its type byte -- a GPT-only check would silently pass
    // every legacy partition through.
    k_memset(&pe, 0, sizeof pe);
    k_memcpy(pe.gpt_type_guid, TYPE_ESP, 16);   // ignored for an MBR
    pe.mbr_type = 0x83;
    KTEST_ASSERT(!partition_is_firmware(&pe, PART_TABLE_MBR));
    pe.mbr_type = 0xEF;
    KTEST_ASSERT(partition_is_firmware(&pe, PART_TABLE_MBR));
}

KTEST("partition", "validate refuses partitions that overlap each other") {
    static struct partition_table t;
    k_memset(&t, 0, sizeof t);
    t.kind = PART_TABLE_MBR;
    t.entry_count = 2;
    t.entries[0].mbr_lba_start = 2048;  t.entries[0].mbr_num_sectors = 1000;
    t.entries[1].mbr_lba_start = 2500;  t.entries[1].mbr_num_sectors = 1000; // starts inside #1
    const char *why = "";
    KTEST_ASSERT(!partition_validate(&t, &why));
}

KTEST("partition", "validate accepts partitions that merely touch") {
    // The half-open boundary, and the reason the overlap test above is
    // written out rather than as a clever one-liner: an off-by-one in
    // either direction turns "adjacent" into "overlapping" and refuses
    // the most ordinary layout there is.
    static struct partition_table t;
    k_memset(&t, 0, sizeof t);
    t.kind = PART_TABLE_MBR;
    t.entry_count = 2;
    t.entries[0].mbr_lba_start = 2048;  t.entries[0].mbr_num_sectors = 1000;
    t.entries[1].mbr_lba_start = 3048;  t.entries[1].mbr_num_sectors = 1000; // exactly after #1
    const char *why = "";
    KTEST_ASSERT(partition_validate(&t, &why));
}

KTEST("partition", "validate refuses a partition running past the disk") {
    if (!blk_root_present()) KTEST_SKIP("no block device");
    static struct partition_table t;
    k_memset(&t, 0, sizeof t);
    t.kind = PART_TABLE_MBR;
    t.entry_count = 1;
    t.entries[0].mbr_lba_start = blkdev_sector_count(blk_root_disk()) - 10;
    t.entries[0].mbr_num_sectors = 1000;
    const char *why = "";
    KTEST_ASSERT(!partition_validate(&t, &why));
}

KTEST("partition", "validate refuses a partition sitting on the table") {
    // LBA 0..33 is the GPT itself. A table that describes a partition
    // there would be describing its own destruction.
    static struct partition_table t;
    k_memset(&t, 0, sizeof t);
    t.kind = PART_TABLE_GPT;
    t.entry_count = 1;
    t.entries[0].gpt_lba_start = 2;
    t.entries[0].gpt_lba_end = 2047;
    const char *why = "";
    KTEST_ASSERT(!partition_validate(&t, &why));
}

KTEST("partition", "validate refuses a fifth MBR partition") {
    static struct partition_table t;
    k_memset(&t, 0, sizeof t);
    t.kind = PART_TABLE_MBR;
    t.entry_count = 5;
    for (int i = 0; i < 5; i++) {
        t.entries[i].mbr_lba_start = 2048 + (uint32_t)i * 1000;
        t.entries[i].mbr_num_sectors = 1000;
    }
    const char *why = "";
    KTEST_ASSERT(!partition_validate(&t, &why));
}

KTEST("partition", "a GPT round-trips through the writer and the parser") {
    void *scratch = kmalloc(SCRATCH_BYTES);
    if (!scratch) KTEST_SKIP("no memory for a scratch disk");

    // Everything from here until the restore runs with the real block
    // device swapped out. Preemption stays off across all of it.
    scheduler_preempt_disable();
    const struct block_device *saved = blk_root();
    int registered = blk_ram_register((uint64_t)(uintptr_t)scratch, SCRATCH_BYTES);

    int wrote = 0, read_back = 0;
    static struct partition_table in, out;

    if (registered) {
        k_memset(&in, 0, sizeof in);
        in.kind = PART_TABLE_GPT;
        in.entry_count = 2;
        in.entries[0].gpt_lba_start = 64;
        in.entries[0].gpt_lba_end = 511;   // INCLUSIVE -- 448 sectors
        k_strlcpy(in.entries[0].gpt_name, "first", sizeof in.entries[0].gpt_name);
        in.entries[1].gpt_lba_start = 512;
        in.entries[1].gpt_lba_end = 900;
        k_strlcpy(in.entries[1].gpt_name, "second", sizeof in.entries[1].gpt_name);

        partition_fill_defaults(&in);
        wrote = partition_write_table(&in);
        if (wrote) read_back = partition_read_table(&out);
    }

    // RESTORED BEFORE ANY ASSERTION. A failing KTEST_ASSERT returns
    // from the test function, so an assert before this line would leave
    // the machine running on a 512 KiB RAM disk with no filesystem --
    // one red test taking every later test with it, and the boot too.
    blk_register(saved);
    scheduler_preempt_enable();
    kfree(scratch);

    KTEST_ASSERT(registered);
    KTEST_ASSERT(wrote);
    KTEST_ASSERT(read_back);
    KTEST_ASSERT_EQ(out.kind, PART_TABLE_GPT);
    KTEST_ASSERT(out.gpt_header_valid);   // the header CRC32 checked out
    KTEST_ASSERT_EQ(out.entry_count, 2);

    for (int i = 0; i < 2; i++) {
        KTEST_ASSERT_EQ(out.entries[i].gpt_lba_start, in.entries[i].gpt_lba_start);
        KTEST_ASSERT_EQ(out.entries[i].gpt_lba_end, in.entries[i].gpt_lba_end);
        KTEST_ASSERT_EQ(k_strcmp(out.entries[i].gpt_name, in.entries[i].gpt_name), 0);
        // The GUIDs are the mixed-endian half, and the half most likely
        // to be written one way and read another -- which would look
        // perfectly fine in isolation and match nothing.
        KTEST_ASSERT_EQ(k_memcmp(out.entries[i].gpt_type_guid,
                                 in.entries[i].gpt_type_guid, 16), 0);
        KTEST_ASSERT_EQ(k_memcmp(out.entries[i].gpt_unique_guid,
                                 in.entries[i].gpt_unique_guid, 16), 0);
    }
}

KTEST("partition", "an MBR round-trips, and a GPT disk is not read as one") {
    void *scratch = kmalloc(SCRATCH_BYTES);
    if (!scratch) KTEST_SKIP("no memory for a scratch disk");

    scheduler_preempt_disable();
    const struct block_device *saved = blk_root();
    int registered = blk_ram_register((uint64_t)(uintptr_t)scratch, SCRATCH_BYTES);

    int wrote = 0, read_back = 0;
    static struct partition_table in, out;

    if (registered) {
        k_memset(&in, 0, sizeof in);
        in.kind = PART_TABLE_MBR;
        in.entry_count = 2;
        in.entries[0].mbr_lba_start = 64;  in.entries[0].mbr_num_sectors = 448;
        in.entries[1].mbr_lba_start = 512; in.entries[1].mbr_num_sectors = 389;
        partition_fill_defaults(&in);
        wrote = partition_write_table(&in);
        if (wrote) read_back = partition_read_table(&out);
    }

    blk_register(saved);
    scheduler_preempt_enable();
    kfree(scratch);

    KTEST_ASSERT(registered);
    KTEST_ASSERT(wrote);
    KTEST_ASSERT(read_back);
    // MBR, not GPT: the discriminator is the protective 0xEE entry, and
    // a parser that just tried LBA 1 regardless would read whatever was
    // there and could report either answer.
    KTEST_ASSERT_EQ(out.kind, PART_TABLE_MBR);
    KTEST_ASSERT_EQ(out.entry_count, 2);
    KTEST_ASSERT_EQ(out.entries[0].mbr_lba_start, 64);
    KTEST_ASSERT_EQ(out.entries[0].mbr_num_sectors, 448);
    KTEST_ASSERT_EQ(out.entries[1].mbr_lba_start, 512);
}

// ---- editing one entry in place (partition_edit_of) ----------------------
//
// The promise is EVERY OTHER BYTE, so the assertions read the scratch
// disk's raw bytes rather than the parser's view of them: a parser cannot
// see an attribute bit or a disk GUID that an edit regenerated.

#define ARR_BYTES (128 * 128)
#define BACKUP_HDR (SCRATCH_SECTORS - 1)
#define BACKUP_ARR (BACKUP_HDR - ARR_BYTES / 512)

static uint32_t le32_at(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A header whose own CRC holds and whose array CRC matches the array it
// points at -- what a firmware checks before it trusts either copy.
static int header_sound(const uint8_t *disk, uint32_t lba) {
    static uint8_t h[512];
    k_memcpy(h, disk + (uint64_t)lba * 512, 512);
    if (k_memcmp(h, "EFI PART", 8) != 0) return 0;
    uint32_t size = le32_at(h + 12), crc = le32_at(h + 16);
    h[16] = h[17] = h[18] = h[19] = 0;
    if (kcrc32(h, size) != crc) return 0;
    uint32_t arr = le32_at(h + 72);
    return kcrc32(disk + (uint64_t)arr * 512, ARR_BYTES) == le32_at(h + 88);
}

KTEST("partition", "an edit deletes one GPT entry and keeps every other byte") {
    uint8_t *scratch = kmalloc(SCRATCH_BYTES);
    static uint8_t before[ARR_BYTES], guid[16];
    if (!scratch) KTEST_SKIP("no memory for a scratch disk");
    k_memset(scratch, 0, SCRATCH_BYTES);

    scheduler_preempt_disable();
    const struct block_device *saved = blk_root();
    int registered = blk_ram_register((uint64_t)(uintptr_t)scratch, SCRATCH_BYTES);
    static struct partition_table in, out;
    int wrote = 0, del = -1, refused = 0, sound = 0, back_sound = 0, read_back = 0;
    int same_other = 0, same_guid = 0, refusal_kept = 0;

    if (registered) {
        k_memset(&in, 0, sizeof in);
        in.kind = PART_TABLE_GPT;
        in.entry_count = 3;
        uint64_t at[4] = { 64, 300, 500, 900 };
        for (int i = 0; i < 3; i++) {
            in.entries[i].gpt_lba_start = at[i];
            in.entries[i].gpt_lba_end = at[i + 1] - 1;
        }
        partition_fill_defaults(&in);
        wrote = partition_write_table(&in);
        // An ATTRIBUTE bit on entry 3 (offset 48), which this kernel never
        // writes -- the byte a rebuilt table would lose. Both arrays, then
        // both CRCs, by hand.
        if (wrote) {
            const char *why;
            scratch[2 * 512 + 2 * 128 + 48] = 0x04;
            scratch[(uint64_t)BACKUP_ARR * 512 + 2 * 128 + 48] = 0x04;
            uint8_t *ph = scratch + 512, *bh = scratch + (uint64_t)BACKUP_HDR * 512;
            uint32_t crc = kcrc32(scratch + 2 * 512, ARR_BYTES);
            for (int k = 0; k < 2; k++) {
                uint8_t *h = k ? bh : ph;
                h[88] = (uint8_t)crc; h[89] = (uint8_t)(crc >> 8); h[90] = (uint8_t)(crc >> 16); h[91] = (uint8_t)(crc >> 24);
                h[16] = h[17] = h[18] = h[19] = 0;
                uint32_t hc = kcrc32(h, 92);
                h[16] = (uint8_t)hc; h[17] = (uint8_t)(hc >> 8); h[18] = (uint8_t)(hc >> 16); h[19] = (uint8_t)(hc >> 24);
            }
            k_memcpy(before, scratch + 2 * 512, ARR_BYTES);
            k_memcpy(guid, scratch + 512 + 56, 16);
            del = partition_edit_of(blk_root_disk(), PART_EDIT_DELETE, 1, 0, &why);
            same_other = k_memcmp(before + 128, scratch + 2 * 512 + 128, ARR_BYTES - 128) == 0;
            same_guid = k_memcmp(guid, scratch + 512 + 56, 16) == 0;
            sound = header_sound(scratch, 1);
            back_sound = header_sound(scratch, BACKUP_HDR);
            // Entry 2 growing over entry 3 must be refused and change NOTHING.
            k_memcpy(before, scratch + 2 * 512, ARR_BYTES);
            refused = partition_edit_of(blk_root_disk(), PART_EDIT_RESIZE, 2, 400, &why) == -EINVAL;
            refusal_kept = k_memcmp(before, scratch + 2 * 512, ARR_BYTES) == 0;
            read_back = partition_read_table(&out);
        }
    }

    blk_register(saved);
    scheduler_preempt_enable();
    kfree(scratch);

    KTEST_ASSERT(registered && wrote);
    KTEST_ASSERT_EQ(del, 0);
    KTEST_ASSERT(same_other);   // entries 2..128, attribute bit included
    KTEST_ASSERT(same_guid);    // the disk GUID, which the whole-table writer regenerates
    KTEST_ASSERT(sound);
    KTEST_ASSERT(back_sound);
    KTEST_ASSERT(refused);
    KTEST_ASSERT(refusal_kept);
    KTEST_ASSERT(read_back);
    KTEST_ASSERT_EQ(out.entry_count, 2);
    // The SLOTS survive the hole: what was partition 2 is still 2.
    KTEST_ASSERT_EQ(out.entries[0].slot, 2);
    KTEST_ASSERT_EQ(out.entries[1].slot, 3);
    KTEST_ASSERT_EQ(out.entries[0].gpt_lba_start, 300);
}

KTEST("partition", "an edit resizes a GPT entry's end and an MBR slot keeps its number") {
    uint8_t *scratch = kmalloc(SCRATCH_BYTES);
    if (!scratch) KTEST_SKIP("no memory for a scratch disk");
    k_memset(scratch, 0, SCRATCH_BYTES);

    scheduler_preempt_disable();
    const struct block_device *saved = blk_root();
    int registered = blk_ram_register((uint64_t)(uintptr_t)scratch, SCRATCH_BYTES);
    static struct partition_table in, gpt, mbr;
    int grew = -1, past = 0, read_gpt = 0, mdel = -1, mgrow = -1, mover = 0, read_mbr = 0;

    if (registered) {
        const char *why;
        k_memset(&in, 0, sizeof in);
        in.kind = PART_TABLE_GPT;
        in.entry_count = 1;
        in.entries[0].gpt_lba_start = 64;
        in.entries[0].gpt_lba_end = 299;
        partition_fill_defaults(&in);
        if (partition_write_table(&in)) {
            grew = partition_edit_of(blk_root_disk(), PART_EDIT_RESIZE, 1, 600, &why);
            // The last usable block is BACKUP_ARR - 1: one past it is refused.
            past = partition_edit_of(blk_root_disk(), PART_EDIT_RESIZE, 1,
                                     (uint64_t)BACKUP_ARR - 64 + 1, &why) == -EINVAL;
            read_gpt = partition_read_table(&gpt);
        }

        k_memset(scratch, 0, SCRATCH_BYTES);
        k_memset(&in, 0, sizeof in);
        in.kind = PART_TABLE_MBR;
        in.entry_count = 3;
        uint32_t at[4] = { 64, 300, 500, 900 };
        for (int i = 0; i < 3; i++) {
            in.entries[i].mbr_lba_start = at[i];
            in.entries[i].mbr_num_sectors = at[i + 1] - at[i];
        }
        partition_fill_defaults(&in);
        if (partition_write_table(&in)) {
            mdel = partition_edit_of(blk_root_disk(), PART_EDIT_DELETE, 2, 0, &why);
            mgrow = partition_edit_of(blk_root_disk(), PART_EDIT_RESIZE, 1, 436, &why);   // into the hole
            mover = partition_edit_of(blk_root_disk(), PART_EDIT_RESIZE, 1, 437, &why) == -EINVAL;
            read_mbr = partition_read_table(&mbr);
        }
    }

    blk_register(saved);
    scheduler_preempt_enable();
    kfree(scratch);

    KTEST_ASSERT(registered);
    KTEST_ASSERT_EQ(grew, 0);
    KTEST_ASSERT(past);
    KTEST_ASSERT(read_gpt);
    KTEST_ASSERT(gpt.gpt_header_valid);
    KTEST_ASSERT_EQ(gpt.entries[0].gpt_lba_end, 663);   // 64 + 600 - 1, inclusive
    KTEST_ASSERT_EQ(mdel, 0);
    KTEST_ASSERT_EQ(mgrow, 0);
    KTEST_ASSERT(mover);   // one sector more would sit on slot 3
    KTEST_ASSERT(read_mbr);
    KTEST_ASSERT_EQ(mbr.entry_count, 2);
    KTEST_ASSERT_EQ(mbr.entries[0].mbr_num_sectors, 436);
    KTEST_ASSERT_EQ(mbr.entries[1].slot, 3);
}
