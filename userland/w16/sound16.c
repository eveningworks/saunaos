// sound16.c -- a Win16 program's sounds, through SaunaOS's sound daemon
// (lib/usnd.h, daemon only: w16 gave up the `device` promise):
//
//   MMSYSTEM's sndPlaySound -- a WAV in memory (Hearts' own, from its
//   resources), a file, or an event's ALIAS -- and waveOutGetNumDevs,
//   which says whether there is a device at all (Hearts asks first).
//   USER's MessageBeep: the event its type names (MB_ICONHAND is Critical
//   Stop...), as 3.1 played them; a MessageBox plays its icon's.
//   SOUND.DRV's notes (Minesweeper's): BASIC's PLAY, played as the PC
//   speaker played them -- a square wave.
//
// WHOSE SOUND AN EVENT IS (the `sounds` setting, w16prefs.h): what
// WIN.INI's [sounds] names on drive C:, where the file is there -- the
// user's own Windows 3.11 sounds, never shipped with SaunaOS -- else
// SaunaOS's own (/usr/share/sounds); SaunaOS's own always; or none, Off
// answering that there is no wave device.
//
// THE BYTES ARE THE PROGRAM'S: a WAV handed over in memory is read with
// every length checked against what the segment holds.
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "lib/usnd.h"
#include "w16/w16prefs.h"

#define SND_ASYNC     0x0001
#define SND_NODEFAULT 0x0002
#define SND_MEMORY    0x0004
#define SND_LOOP      0x0008
#define SND_NOSTOP    0x0010

static int g_tried, g_ok;                        // usnd_init() asked once
static struct usnd_clip g_clip;                  // MMSYSTEM's one sound at a time
static usnd_voice_t g_voice;
static int g_loop;                               // SND_LOOP: started again as it ends
static uint64_t g_until_ms;                      // when the sound now playing ends

static int ready(struct w16 *w) {
    (void)w;
    if (g_w16_prefs.sounds == W16_SOUNDS_OFF) return 0;
    if (!g_tried) {
        g_tried = 1;
        usnd_set_daemon_only(1);
        g_ok = usnd_init() == 0;
    }
    return g_ok;
}

void w16_sound_set_name(const char *name) { usnd_set_app_name(name); }

// --- WAV ---------------------------------------------------------------------------

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

// A RIFF WAVE of PCM (8 or 16 bits, mono or stereo) as a clip; -1 for
// anything else or anything cut short.
static int wav_clip(const uint8_t *b, uint32_t n, struct usnd_clip *out) {
    if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return -1;
    uint32_t at = 12, rate = 0;
    unsigned ch = 0, bits = 0, fmt = 0;
    const uint8_t *data = 0;
    uint32_t len = 0;
    while (at + 8 <= n) {
        uint32_t sz = le32(b + at + 4);
        const uint8_t *c = b + at + 8;
        if (sz > n - at - 8) sz = n - at - 8;    // a data chunk longer than the block: what is there
        if (!memcmp(b + at, "fmt ", 4) && sz >= 16) {
            fmt = le16(c); ch = le16(c + 2); rate = le32(c + 4); bits = le16(c + 14);
        } else if (!memcmp(b + at, "data", 4)) {
            data = c; len = sz;
        }
        at += 8 + sz + (sz & 1);
    }
    if (fmt != 1 || !data || (ch != 1 && ch != 2) || (bits != 8 && bits != 16) || rate < 1000 || rate > 192000)
        return -1;
    uint64_t frames = len / (ch * (bits / 8));
    if (!frames || frames > (uint64_t)rate * 30) return -1;
    int16_t *pcm = malloc((size_t)frames * ch * 2);
    if (!pcm) return -1;
    for (uint64_t i = 0; i < frames * ch; i++)
        pcm[i] = bits == 8 ? (int16_t)((data[i] - 128) << 8) : (int16_t)le16(data + 2 * i);
    int r = usnd_clip_from_pcm(pcm, frames, rate, (int)ch, out);
    free(pcm);
    return r == 0 ? 0 : -1;
}

static int file_clip(const char *host, struct usnd_clip *out) {
    FILE *f = fopen(host, "rb");
    if (!f) return -1;
    uint8_t *buf = 0;
    long sz = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    int r = -1;
    if (sz > 0 && sz <= (16 << 20) && fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)sz)) &&
        fread(buf, 1, (size_t)sz, f) == (size_t)sz)
        r = wav_clip(buf, (uint32_t)sz, out);
    free(buf);
    fclose(f);
    return r;
}

// --- events ---------------------------------------------------------------------------

// SaunaOS's own sound for an event: the logon one for Windows Start and
// Exit, the chime for every other (lib's generated set, data/usr/share/sounds).
static const char *saunaos_sound(const char *alias) {
    if (alias && (!strcasecmp(alias, "SystemStart") || !strcasecmp(alias, "SystemExit")))
        return "/usr/share/sounds/win.wav";
    return "/usr/share/sounds/chime.wav";
}

int w16_sound_resolve(const char *alias, char *ini, size_t ini_cap, char *host, size_t host_cap) {
    ini[0] = host[0] = 0;
    if (g_w16_prefs.sounds == W16_SOUNDS_OFF) return W16_SND_OFF;
    if (g_w16_prefs.sounds == W16_SOUNDS_WIN311 && alias &&
        w16_ini_get("WIN.INI", "sounds", alias, ini, ini_cap) > 0) {
        char file[128];
        snprintf(file, sizeof file, "%s", ini);
        char *comma = strchr(file, ',');
        if (comma) *comma = 0;
        for (char *e = file + strlen(file); e > file && e[-1] == ' '; ) *--e = 0;
        char dos[300];
        // A bare name is in C:\WINDOWS, as 3.1 looked for it.
        if (strchr(file, '\\') || strchr(file, ':')) snprintf(dos, sizeof dos, "%s", file);
        else snprintf(dos, sizeof dos, "C:\\WINDOWS\\%s", file);
        FILE *f;
        if (file[0] && w16_dos_path(dos, host, host_cap, 0, 0, 0) == 0 && (f = fopen(host, "rb"))) {
            fclose(f);
            return W16_SND_FOUND;
        }
        snprintf(host, host_cap, "%s", saunaos_sound(alias));
        return W16_SND_MISSING;
    }
    snprintf(host, host_cap, "%s", saunaos_sound(alias));
    return W16_SND_SAUNAOS;
}

static void stop(void) {
    g_loop = 0;
    if (g_voice != USND_VOICE_NONE) usnd_voice_stop(g_voice);
    g_voice = USND_VOICE_NONE;
    usnd_clip_free(&g_clip);
    memset(&g_clip, 0, sizeof g_clip);
}

// `c` played on MMSYSTEM's voice (taking it over); SND_SYNC waits it out.
static int play(struct usnd_clip *c, unsigned flags) {
    if (!(flags & SND_NOSTOP)) stop();
    else if (g_voice != USND_VOICE_NONE && usnd_voice_active(g_voice)) { usnd_clip_free(c); return 0; }
    else stop();
    g_clip = *c;
    g_voice = usnd_voice_play(&g_clip, 256, 256);
    if (g_voice == USND_VOICE_NONE) return 0;
    uint64_t ms = g_clip.frames * 1000 / USND_RATE;
    g_until_ms = w16_now_ms() + ms;
    g_loop = (flags & (SND_LOOP | SND_ASYNC)) == (SND_LOOP | SND_ASYNC);
    if (!(flags & SND_ASYNC)) {
        struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000 };
        nanosleep(&ts, 0);
    }
    return 1;
}

int w16_sound_event(struct w16 *w, const char *alias) {
    if (!ready(w)) return 0;
    char ini[160], host[600];
    w16_sound_resolve(alias, ini, sizeof ini, host, sizeof host);
    struct usnd_clip c;
    if (file_clip(host, &c) < 0) return 0;
    return play(&c, SND_ASYNC);
}

void w16_sound_tick(void) {
    if (!g_loop || g_voice == USND_VOICE_NONE || w16_now_ms() < g_until_ms || usnd_voice_active(g_voice)) return;
    g_voice = usnd_voice_play(&g_clip, 256, 256);
    g_until_ms = w16_now_ms() + g_clip.frames * 1000 / USND_RATE;
}

// MessageBeep's type -> the event 3.1 played for it.
const char *w16_beep_alias(unsigned type) {
    switch (type & 0xF0) {
    case 0x10: return "SystemHand";              // MB_ICONHAND: Critical Stop
    case 0x20: return "SystemQuestion";
    case 0x30: return "SystemExclamation";
    case 0x40: return "SystemAsterisk";
    default:   return "SystemDefault";           // MB_OK, and -1's speaker beep
    }
}

// --- MMSYSTEM -----------------------------------------------------------------------------

// sndPlaySound(lpszSound, fuSound) -> BOOL
// Windows: Plays a WAV file, a sound event by its WIN.INI name (SystemAsterisk...) or, with
// SND_MEMORY, a WAV already in memory -- at once (SND_ASYNC) or before returning, looping with
// SND_LOOP; NULL stops what plays. When nothing is found it plays the Default Beep, unless
// SND_NODEFAULT.
// w16: The same through SaunaOS's sound daemon, one sound at a time as MMSYSTEM did. An event plays
// the file drive C:'s WIN.INI names in [sounds] when the file is there -- the user's own Windows
// 3.11 sounds -- else SaunaOS's own; the Sounds setting may choose SaunaOS's always, or none. The WAV
// is 8- or 16-bit PCM, read with every size checked; anything else does not play.
static int api_sndPlaySound(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t far = w16_arg32(a);
    uint16_t flags = w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    if (!(far >> 16)) { stop(); w->cpu.r[R_AX] = 1; return 6; }
    if (!ready(w)) return 6;
    struct usnd_clip c;
    if (flags & SND_MEMORY) {
        uint16_t sel = (uint16_t)(far >> 16), off = (uint16_t)far;
        const uint8_t *h = cpu16_ptr(&w->cpu, sel, off, 8, 0);
        if (!h) return -1;
        uint32_t n = le32(h + 4) + 8;            // the RIFF chunk, header included
        const struct cpu16_desc *d = &w->desc[cpu16_index(sel) % CPU16_MAX_DESC];
        if (off + (uint64_t)n > (uint64_t)d->limit + 1) n = d->limit + 1u - off;   // what the block holds
        const uint8_t *b = cpu16_ptr(&w->cpu, sel, off, n, 0);
        if (!b) return -1;
        if (wav_clip(b, n, &c) < 0) return 6;
    } else {
        char name[160], ini[160], host[600];
        if (cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, name, sizeof name) < 0) return -1;
        int found = g_w16_prefs.sounds == W16_SOUNDS_WIN311 && w16_ini_get("WIN.INI", "sounds", name, ini, sizeof ini) > 0;
        if (found) w16_sound_resolve(name, ini, sizeof ini, host, sizeof host);
        else if (w16_dos_path(name, host, sizeof host, 0, 0, 0) != 0) {
            if (flags & SND_NODEFAULT) return 6;
            w16_sound_resolve("SystemDefault", ini, sizeof ini, host, sizeof host);
        }
        if (file_clip(host, &c) < 0) return 6;
    }
    w->cpu.r[R_AX] = (uint16_t)play(&c, flags);
    return 6;
}

// waveOutGetNumDevs() -> UINT
// Windows: How many wave output devices -- sound cards -- there are.
// w16: 1 when SaunaOS's sound daemon is running and sounds are on, else 0: a program then knows not
// to try.
static int api_waveOutGetNumDevs(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    w->cpu.r[R_AX] = (uint16_t)ready(w);
    return 0;
}

const struct w16_api g_mmsystem_api[] = {
    { 2, api_sndPlaySound },
    { 401, api_waveOutGetNumDevs },
};
const unsigned g_mmsystem_api_n = sizeof g_mmsystem_api / sizeof g_mmsystem_api[0];

// --- SOUND.DRV: BASIC's PLAY on the PC speaker ------------------------------------------------

#define NOTE_RATE 22050

// 2 to the k/12: a note k semitones from A440, by steps (no libm here).
static double semitones(int k) {
    double f = 1.0;
    for (; k > 0; k--) f *= 1.0594630943592953;
    for (; k < 0; k++) f /= 1.0594630943592953;
    return f;
}

// The voice's queue holds notes (SetVoiceNote) and sounds (SetVoiceSound, `sound` set): six bytes
// an entry, as SOUND.DRV counts them -- 192 bytes, 32 entries, until SetVoiceQueueSize; a full one
// answers S_SERQFUL (measured on the reference VM, 2026-11-01).
#define QUEUE_ENTRY 6
#define QUEUE_DEFAULT (192 / QUEUE_ENTRY)
#define S_SERQFUL 0xFFFC
struct note { uint8_t value, length, dots, sound; uint16_t ticks; uint32_t hz16; };
static struct note *g_notes;
static int g_nnotes, g_cap, g_qmax = QUEUE_DEFAULT;
static int g_tempo = 120, g_mode, g_pitch;       // SetVoiceAccent's; S_NORMAL
static struct usnd_clip g_tune;
static usnd_voice_t g_tune_voice;
static uint64_t g_tune_end_ms;                   // when what StartSound played ends

// One more entry on the queue; 0 when it is full or memory is.
static struct note *queue_add(void) {
    if (g_nnotes >= g_qmax) return 0;
    if (g_nnotes == g_cap) {
        int cap = g_cap ? g_cap * 2 : 32;
        struct note *g = realloc(g_notes, sizeof *g * (size_t)cap);
        if (!g) return 0;
        g_notes = g;
        g_cap = cap;
    }
    struct note *n = &g_notes[g_nnotes++];
    memset(n, 0, sizeof *n);
    return n;
}

// OpenSound() -> int
// Windows: Opens SOUND.DRV, the PC speaker's music driver, and returns how many voices it has --
// one on a PC -- or S_SERDVNA when another program has it.
// w16: 1 voice, played through the sound card as a square wave, the speaker's sound; S_SERDVNA when
// sounds are off, the Old sound driver setting is, or there is no device.
static int api_OpenSound(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal; (void)a;
    g_qmax = QUEUE_DEFAULT;
    w->cpu.r[R_AX] = g_w16_prefs.sound_drv && ready(w) ? 1 : 0xFFFF;
    return 0;
}

// CloseSound() -> void
// StopSound() -> void
// Windows: StopSound stops the notes playing and empties the queue; CloseSound does that and closes
// the driver.
// w16: Both stop what plays and empty the queue.
static int api_StopSound(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)a;
    if (g_tune_voice != USND_VOICE_NONE) usnd_voice_stop(g_tune_voice);
    g_tune_voice = USND_VOICE_NONE;
    g_nnotes = 0;
    if (ordinal == 2) { usnd_clip_free(&g_tune); memset(&g_tune, 0, sizeof g_tune); }
    w->cpu.r[R_AX] = 0;
    return 0;
}

// SetVoiceNote(nVoice, nValue, nLength, nCdots) -> int
// Windows: Queues a note, BASIC's PLAY in a call: nValue 1 to 84 is seven octaves from the C three
// below middle C (37 is middle C), 0 a rest; nLength 1 a whole note, 4 a quarter; each dot half as
// long again.
// w16: The same, queued until StartSound.
static int api_SetVoiceNote(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    uint16_t value = w16_arg16(a), length = w16_arg16(a), dots = w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    if (value > 84 || length < 1 || length > 64) { w->cpu.r[R_AX] = 0xFFFF; return 8; }
    struct note *n = queue_add();
    if (!n) { w->cpu.r[R_AX] = S_SERQFUL; return 8; }
    *n = (struct note){ (uint8_t)value, (uint8_t)length, (uint8_t)(dots < 3 ? dots : 3), 0, 0, 0 };
    return 8;
}

// SetVoiceQueueSize(nVoice, nBytes) -> int
// Windows: Sizes the voice's queue, six bytes a note or sound; 192 bytes until it is called, and
// again after CloseSound.
// w16: The same count: nBytes / 6 entries, the queue emptied.
static int api_SetVoiceQueueSize(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    uint16_t bytes = w16_arg16(a);
    g_qmax = bytes / QUEUE_ENTRY;
    g_nnotes = 0;
    w->cpu.r[R_AX] = 0;
    return 4;
}

// SetVoiceSound(nVoice, dwFrequency, nDuration) -> int
// Windows: Queues a tone: the frequency in hertz in the high word, a fraction of one in the low;
// nDuration in the driver's ticks. Tempo and SetVoiceAccent's mode do not touch it.
// w16: The same, a square wave at that frequency for nDuration + 1 ticks of 2.44 ms -- 3.11's
// length, measured on the reference VM (2026-11-01: 100 one-tick sounds 494 ms, ten of 18 ticks
// 439 ms, one of 180 ticks 439 ms, at any tempo; GetTickCount's 55 ms the resolution).
static int api_SetVoiceSound(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    uint32_t hz16 = w16_arg32(a);
    uint16_t ticks = w16_arg16(a);
    struct note *n = queue_add();
    if (!n) { w->cpu.r[R_AX] = S_SERQFUL; return 8; }
    n->sound = 1;
    n->ticks = ticks;
    n->hz16 = hz16;                              // 0 Hz: a rest
    w->cpu.r[R_AX] = 0;
    return 8;
}

// SetVoiceAccent(nVoice, nTempo, nVolume, nMode, nPitch) -> int
// Windows: Sets how the notes after it play: the tempo in quarter notes a minute (32 to 255),
// legato, normal or staccato, and a transposition in semitones.
// w16: The same: normal plays seven eighths of a note's length, staccato three quarters, legato all
// of it.
static int api_SetVoiceAccent(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    uint16_t tempo = w16_arg16(a);
    w16_arg16(a);                                // volume: a speaker has one
    uint16_t mode = w16_arg16(a), pitch = w16_arg16(a);
    if (tempo >= 32 && tempo <= 255) g_tempo = tempo;
    if (mode <= 2) g_mode = mode;
    if (pitch <= 83) g_pitch = pitch;
    w->cpu.r[R_AX] = 0;
    return 10;
}

// StartSound() -> int
// Windows: Starts playing the queued notes, in the background.
// w16: Renders the queue into one square wave at its tempo and plays it through the sound daemon.
static int api_StartSound(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal; (void)a;
    w->cpu.r[R_AX] = 0;
    if (!g_nnotes || !g_w16_prefs.sound_drv || !ready(w)) { g_nnotes = 0; return 0; }
    uint64_t total = 0;
    uint32_t *len = malloc(sizeof *len * (size_t)g_nnotes);
    if (!len) return 0;
    for (int i = 0; i < g_nnotes; i++) {
        if (g_notes[i].sound) {
            len[i] = (uint32_t)((g_notes[i].ticks + 1u) * (uint64_t)NOTE_RATE * 244 / 100000);
        } else {
            // A whole note is four quarters; each dot adds half of the last part.
            double q = 4.0 / g_notes[i].length, part = q;
            for (int d = 0; d < g_notes[i].dots; d++) { part /= 2; q += part; }
            len[i] = (uint32_t)(q * 60.0 / g_tempo * NOTE_RATE);
        }
        total += len[i];
    }
    int16_t *pcm = total && total < (uint64_t)NOTE_RATE * 60 ? calloc((size_t)total, 2) : 0;
    if (pcm) {
        uint64_t at = 0;
        for (int i = 0; i < g_nnotes; i++) {
            if (g_notes[i].sound) {
                double f = (g_notes[i].hz16 >> 16) + (g_notes[i].hz16 & 0xFFFF) / 65536.0;
                double half = NOTE_RATE / f / 2;
                for (uint32_t k = 0; g_notes[i].hz16 && k < len[i]; k++)
                    pcm[at + k] = (int16_t)(((uint64_t)(k / half) & 1) ? -6000 : 6000);
                at += len[i];
                continue;
            }
            // Normal sounds 7/8 of the note, staccato 3/4, legato all of it.
            uint32_t on = g_mode == 1 ? len[i] : g_mode == 2 ? len[i] * 3 / 4 : len[i] * 7 / 8;
            if (g_notes[i].value) {
                int midi = g_notes[i].value + 23 + g_pitch;            // 37 + 23 = 60, middle C
                double f = 440.0 * semitones(midi - 69);
                double half = NOTE_RATE / f / 2;
                for (uint32_t k = 0; k < on; k++) pcm[at + k] = (int16_t)(((int)(k / half) & 1) ? -6000 : 6000);
            }
            at += len[i];
        }
        if (g_tune_voice != USND_VOICE_NONE) usnd_voice_stop(g_tune_voice);
        usnd_clip_free(&g_tune);
        if (usnd_clip_from_pcm(pcm, total, NOTE_RATE, 1, &g_tune) == 0) {
            g_tune_voice = usnd_voice_play(&g_tune, 256, 256);
            g_tune_end_ms = w16_now_ms() + total * 1000 / NOTE_RATE;
        }
    }
    free(pcm);
    free(len);
    g_nnotes = 0;
    return 0;
}

// WaitSoundState(fnState) -> int
// Windows: Waits until the voice's queue is empty (S_QUEUEEMPTY) or below its threshold
// (S_THRESHOLD, S_ALLTHRESHOLD); S_SERDST for any other state. The whole system waited with it.
// w16: Sleeps until what StartSound played has ended -- no threshold is ever set, so all three
// wait for the end. 0 at once when nothing plays.
static int api_WaitSoundState(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t state = w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    if (state > 2) { w->cpu.r[R_AX] = 0xFFF0; return 2; }       // S_SERDST
    uint64_t now = w16_now_ms();
    if (g_tune_voice != USND_VOICE_NONE && g_tune_end_ms > now) {
        uint64_t ms = g_tune_end_ms - now;
        struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000 };
        nanosleep(&ts, 0);
    }
    return 2;
}

const struct w16_api g_sound_api[] = {
    { 1, api_OpenSound },
    { 2, api_StopSound },        // CloseSound
    { 3, api_SetVoiceQueueSize },
    { 4, api_SetVoiceNote },
    { 5, api_SetVoiceAccent },
    { 8, api_SetVoiceSound },
    { 9, api_StartSound },
    { 10, api_StopSound },
    { 11, api_WaitSoundState },
};
const unsigned g_sound_api_n = sizeof g_sound_api / sizeof g_sound_api[0];
