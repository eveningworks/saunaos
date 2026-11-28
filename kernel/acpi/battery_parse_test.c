// _BIX/_BIF/_BST parsing against hand-built packages: the units, the
// unknowns and the shapes a parser must refuse.
#include "battery_parse.h"
#include "ktest.h"
#include "string.h"

#define N(x)  { 0, (x), "" }
#define T(s)  { 1, 0, s }
#define UNK   N(0xFFFFFFFFull)

// A _BIX in mWh, revision 0: the ASUS's shape.
static const struct bat_val BIX_MWH[] = {
    N(0), N(0), N(45000), N(38200), N(1), N(11400), N(4500), N(1500), N(212),
    N(100000), UNK, UNK, UNK, UNK, N(10), N(10),
    T("C31N1411"), T("2215"), T("LION"), T("ASUSTeK"),
};

// A _BIF in mAh: 3950 mAh at 11.4 V is 45030 mWh.
static const struct bat_val BIF_MAH[] = {
    N(1), N(3950), N(3400), N(1), N(11400), N(395), N(120), N(10), N(10),
    T("PA5109U"), N(4711), T("Li-ion"), T("Toshiba"),
};

KTEST("battery", "a _BIX in mWh keeps its numbers, cycles and texts") {
    struct bat_info b;
    KTEST_ASSERT_EQ(bat_parse_bix(BIX_MWH, 20, &b), 0);
    KTEST_ASSERT_EQ(b.mah, 0);
    KTEST_ASSERT_EQ(b.design_mwh, 45000);
    KTEST_ASSERT_EQ(b.full_mwh, 38200);
    KTEST_ASSERT_EQ(b.cycles, 212);
    KTEST_ASSERT_EQ(b.rechargeable, 1);
    KTEST_ASSERT(!k_strcmp(b.model, "C31N1411"));
    KTEST_ASSERT(!k_strcmp(b.maker, "ASUSTeK"));
}

KTEST("battery", "a _BIF in mAh converts through the design voltage, an Integer serial is written out") {
    struct bat_info b;
    KTEST_ASSERT_EQ(bat_parse_bif(BIF_MAH, 13, &b), 0);
    KTEST_ASSERT_EQ(b.mah, 1);
    KTEST_ASSERT_EQ(b.design_mwh, 45030);
    KTEST_ASSERT_EQ(b.full_mwh, 38760);
    KTEST_ASSERT_EQ(b.cycles, BAT_UNKNOWN);
    KTEST_ASSERT(!k_strcmp(b.serial, "4711"));
    // _BST in mA/mAh, converted the same way: 535 mA, 2610 mAh.
    const struct bat_val bst[] = { N(1), N(535), N(2610), N(11100) };
    struct bat_status s;
    KTEST_ASSERT_EQ(bat_parse_bst(bst, 4, &b, &s), 0);
    KTEST_ASSERT_EQ(s.state, BAT_DISCHARGING);
    KTEST_ASSERT_EQ(s.rate_mw, 6099);
    KTEST_ASSERT_EQ(s.now_mwh, 29754);
    KTEST_ASSERT_EQ(bat_percent(&b, &s), 77);
}

KTEST("battery", "an unknown rate stays unknown and the percentage still comes out") {
    struct bat_info b;
    KTEST_ASSERT_EQ(bat_parse_bix(BIX_MWH, 20, &b), 0);
    const struct bat_val bst[] = { N(2), UNK, N(29800), N(12100) };
    struct bat_status s;
    KTEST_ASSERT_EQ(bat_parse_bst(bst, 4, &b, &s), 0);
    KTEST_ASSERT_EQ(s.state, BAT_CHARGING);
    KTEST_ASSERT_EQ(s.rate_mw, BAT_UNKNOWN);
    KTEST_ASSERT_EQ(bat_percent(&b, &s), 78);
}

KTEST("battery", "a negative 16-bit rate is the firmware's bug: its magnitude is the rate") {
    struct bat_info b;
    KTEST_ASSERT_EQ(bat_parse_bix(BIX_MWH, 20, &b), 0);
    const struct bat_val bst[] = { N(1), N(0xE82C), N(29800), N(12100) };   // -6100
    struct bat_status s;
    KTEST_ASSERT_EQ(bat_parse_bst(bst, 4, &b, &s), 0);
    KTEST_ASSERT_EQ(s.rate_mw, 6100);
}

KTEST("battery", "an unknown last-full falls back to the design capacity; over full reads 100") {
    struct bat_info b;
    KTEST_ASSERT_EQ(bat_parse_bix(BIX_MWH, 20, &b), 0);
    b.full_mwh = BAT_UNKNOWN;
    struct bat_status s = { 0, 0, 22500, 0 };
    KTEST_ASSERT_EQ(bat_percent(&b, &s), 50);
    s.now_mwh = 50000;
    KTEST_ASSERT_EQ(bat_percent(&b, &s), 100);
    s.now_mwh = BAT_UNKNOWN;
    KTEST_ASSERT_EQ(bat_percent(&b, &s), -1);
}

KTEST("battery", "short packages, text in a number field and a bad unit are REFUSED") {
    struct bat_info b;
    KTEST_ASSERT_EQ(bat_parse_bix(BIX_MWH, 19, &b), -1);
    KTEST_ASSERT_EQ(bat_parse_bif(BIF_MAH, 12, &b), -1);
    struct bat_val bad[20];
    k_memcpy(bad, BIX_MWH, sizeof bad);
    bad[3] = (struct bat_val)T("38200");
    KTEST_ASSERT_EQ(bat_parse_bix(bad, 20, &b), -1);
    k_memcpy(bad, BIX_MWH, sizeof bad);
    bad[1] = (struct bat_val)N(2);
    KTEST_ASSERT_EQ(bat_parse_bix(bad, 20, &b), -1);
    KTEST_ASSERT_EQ(bat_parse_bix(BIX_MWH, 20, &b), 0);
    const struct bat_val bst[] = { N(1), N(1), N(1) };
    struct bat_status s;
    KTEST_ASSERT_EQ(bat_parse_bst(bst, 3, &b, &s), -1);
}

KTEST("battery", "mAh with no voltage anywhere leaves the energy unknown, not wrong") {
    struct bat_val v[13];
    k_memcpy(v, BIF_MAH, sizeof v);
    v[4] = (struct bat_val)UNK;
    struct bat_info b;
    KTEST_ASSERT_EQ(bat_parse_bif(v, 13, &b), 0);
    KTEST_ASSERT_EQ(b.design_mwh, BAT_UNKNOWN);
    const struct bat_val bst[] = { N(1), N(535), N(2610), N(11100) };
    struct bat_status s;
    KTEST_ASSERT_EQ(bat_parse_bst(bst, 4, &b, &s), 0);
    KTEST_ASSERT_EQ(s.now_mwh, 28971);   // through the present voltage instead
}
