// See api/termkey.h. Compiled TWICE -- once into the kernel, once into
// libuapp.a -- so the terminal that encodes and the editor that decodes
// are the same table, and a key added here reaches both rings at once.
#include "termkey.h"
#include "termkey_cases.h"
#include "keyboard.h"   // IS_UNICODE_CHAR: a KEY_* is never a character
#include "kutf8.h"

int termkey_encode(int key, char *out, int cap) {
    if (!out || cap <= 0) return 0;

    // An ASCII character is itself -- one byte. That includes the
    // control codes: Ctrl-A really is 0x01 on a terminal, and Alt-<key>
    // really is ESC then the key, so neither needs a table row.
    if (key >= 0 && key < 0x80) { out[0] = (char)key; return 1; }

    for (int i = 0; i < TERMKEY_CASE_COUNT; i++) {
        if (TERMKEY_CASES[i].key != key) continue;
        const char *s = TERMKEY_CASES[i].seq;
        int n = 0;
        while (s[n]) n++;
        if (n > cap) return 0;          // REFUSED, never truncated
        for (int j = 0; j < n; j++) out[j] = s[j];
        return n;
    }
    // ANY OTHER CHARACTER IS ITS UTF-8 -- Linux's Unicode keyboard mode
    // (`kbd_mode -u`), which is what a program reading a terminal expects
    // and what every name on the disk is. A KEY_* special that has no
    // sequence (Super, a modifier press) is in the Private Use Area and
    // is refused here rather than sent as three bytes of nonsense.
    if (IS_UNICODE_CHAR(key)) {
        char u[4];
        int n = kutf8_encode((uint32_t)key, u);
        if (n <= 0 || n > cap) return 0;   // not a character, or no room
        for (int j = 0; j < n; j++) out[j] = u[j];
        return n;
    }
    // A key with no terminal representation -- Super, a modifier press.
    // Sending nothing is right: a program reading a terminal could not
    // have acted on it anyway.
    return 0;
}

const char *termkey_pending(const struct termkey_state *st, int *len) {
    if (len) *len = st ? st->len : 0;
    return st ? st->pending : "";
}

// Does `st->pending` match a case exactly, or could it still grow into
// one? Returns the key, TERMKEY_MORE, or TERMKEY_NONE.
static int match(const struct termkey_state *st) {
    int partial = 0;
    for (int i = 0; i < TERMKEY_CASE_COUNT; i++) {
        const char *s = TERMKEY_CASES[i].seq;
        int n = 0;
        while (s[n]) n++;
        if (n < st->len) continue;
        int j = 0;
        while (j < st->len && s[j] == st->pending[j]) j++;
        if (j != st->len) continue;         // diverged
        if (n == st->len) return TERMKEY_CASES[i].key;
        partial = 1;                        // a prefix of this one
    }
    return partial ? TERMKEY_MORE : TERMKEY_NONE;
}

int termkey_refeed(struct termkey_state *st) {
    if (!st || !st->unconsumed) return 0;
    st->unconsumed = 0;
    return 1;
}

// A UTF-8 sequence in progress ended early: `byte` does not continue it.
// THE LEAD BYTE IS A CHARACTER OF ITS OWN -- its Latin-1 letter, the
// transition rule (api/kutf8.h) -- and `byte` was not consumed. The
// continuation bytes between them, if any, are dropped: they are a
// malformed sequence's middle, which the rule would have turned into C1
// controls nobody types.
static int u8_broken(struct termkey_state *st) {
    int lead = (unsigned char)st->u8[0];
    st->u8_len = 0;
    st->unconsumed = 1;
    return lead;
}

int termkey_feed(struct termkey_state *st, int byte) {
    if (!st) return TERMKEY_NONE;
    byte &= 0xFF;

    // **A UTF-8 SEQUENCE IS ONE KEY**, assembled here so a reader gets
    // `ä` and not 0xC3 then 0xA4 -- two keys a line editor would insert
    // as two characters. The bytes arrive one read at a time, so the
    // state is the caller's, beside the ANSI state below.
    if (st->u8_len) {
        if ((byte & 0xC0) != 0x80) return u8_broken(st);
        st->u8[st->u8_len++] = (char)byte;
        if (st->u8_len < st->u8_want) return TERMKEY_MORE;
        int n;
        uint32_t cp = kutf8_decode(st->u8, st->u8_len, &n);
        int lead = (unsigned char)st->u8[0];
        st->u8_len = 0;
        // Overlong, a surrogate, past U+10FFFF: the lead as its letter.
        return n == st->u8_want ? (int)cp : lead;
    }

    if (st->len == 0) {
        if (byte >= 0x80) {
            int want = kutf8_seq_len((unsigned char)byte);
            if (want == 1) return byte;      // a stray byte: its Latin-1 letter
            st->u8[0] = (char)byte;
            st->u8_len = 1;
            st->u8_want = want;
            return TERMKEY_MORE;
        }
        if (byte != 0x1B) return byte;      // an ordinary byte, untouched
        st->pending[0] = 0x1B;
        st->len = 1;
        return TERMKEY_MORE;
    }

    // **THE SECOND BYTE DECIDES WHAT THE ESC WAS.** `[` or `O` begins a
    // sequence; anything else means the ESC was the Esc key or a Meta
    // prefix, so give the ESC back and let the caller re-feed the byte.
    // Resolving on the next byte rather than on a timer is the one real
    // difference from a hardware terminal, and it costs Alt-[ -- the
    // same trade readline makes.
    if (st->len == 1 && byte != '[' && byte != 'O') {
        st->len = 0;
        st->unconsumed = 1;
        return 0x1B;
    }

    if (st->len >= TERMKEY_MAX) { st->len = 0; return TERMKEY_NONE; }
    st->pending[st->len++] = (char)byte;

    int r = match(st);
    if (r == TERMKEY_MORE) return TERMKEY_MORE;
    st->len = 0;                            // matched, or gave up
    return r;
}
