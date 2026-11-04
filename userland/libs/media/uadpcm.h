// media/uadpcm.h -- ADPCM audio decoded to 16-bit PCM: Microsoft's
// (WAVE_FORMAT_ADPCM, 2) and IMA's (WAVE_FORMAT_IMA_ADPCM, 0x11), the two
// a WAV file or Windows' ACM carried, a block at a time. No allocation;
// the format block is untrusted and every length is checked.
#ifndef UADPCM_H
#define UADPCM_H

#include <stddef.h>
#include <stdint.h>

#define UADPCM_MS  2
#define UADPCM_IMA 0x11

struct uadpcm {
    int format;                  // UADPCM_MS or UADPCM_IMA
    int channels;                // 1 or 2
    uint32_t rate;
    unsigned block_align;        // bytes a block
    unsigned frames_per_block;   // what a whole block decodes to
    int ncoef;                   // MS: the predictor pairs
    int16_t coef[256][2];
};

// From a WAVEFORMATEX (its cbSize extra bytes included, `len` of them in
// all): 0, or -1 for a format this does not decode or a block that is
// inconsistent with itself.
int uadpcm_init(struct uadpcm *a, const uint8_t *wfx, size_t len);

// One block (`n` bytes: block_align, or fewer for a file's last) as
// interleaved PCM into `out`, room for frames_per_block frames; the
// frames decoded, 0 for a block too short to hold its header.
size_t uadpcm_block(const struct uadpcm *a, const uint8_t *in, size_t n, int16_t *out);

#endif
