/* SaunaOS's SDL audio driver: SDL's device thread feeds media/usnd.h's
 * pushed source (usnd_push), the one DOOM's music uses.
 *
 * THE DEVICE IS usnd's FORMAT, ALWAYS: stereo S16 at USND_RATE. A program
 * asking for anything else gets SDL's own conversion in front of it, which
 * is what SDL does for every backend whose server has one native format
 * (PipeWire's and PulseAudio's drivers ask; WASAPI shared mode is told).
 * usnd mixes this with the process's other sounds and hands the result to
 * the sink -- the card, or soundd -- so an SDL program is one more client
 * of the same library, never a second owner of the card.
 *
 * usnd_push() NEVER BLOCKS, so the wait is here: the thread sleeps until
 * the queue has room for a buffer and no more than LEAD frames are still
 * ahead of the mixer -- a game's sound effect should not wait behind the
 * whole ~170 ms queue. */
#include "SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_PRIVATE

#include "audio/SDL_sysaudio.h"
#include "media/usnd.h"

/* usnd's mixer takes up to 1024 frames a pass (usnd_mix.c's MIX_FRAMES):
 * three of those queued keeps it fed through a slow pass, ~64 ms. */
#define LEAD 3072

struct SDL_PrivateAudioData {
    Uint8 *mixbuf;
};

static long lead_frames(const SDL_AudioDevice *device)
{
    long lead = (long)device->sample_frames * 3;
    return lead > LEAD ? lead : LEAD;
}

static bool SAUNAOSAUDIO_WaitDevice(SDL_AudioDevice *device)
{
    const long want = device->sample_frames, lead = lead_frames(device);
    while (!SDL_GetAtomicInt(&device->shutdown)) {
        long queued = USND_PUSH_FRAMES - usnd_push_space();
        if (queued + want <= lead && queued + want <= USND_PUSH_FRAMES) {
            return true;
        }
        /* Half a mixer pass: the queue drains at USND_RATE. */
        SDL_Delay(512 * 1000 / USND_RATE / 2 + 1);
    }
    return true;
}

static bool SAUNAOSAUDIO_PlayDevice(SDL_AudioDevice *device, const Uint8 *buffer, int buflen)
{
    const int16_t *pcm = (const int16_t *)buffer;
    long frames = buflen / (long)(sizeof(int16_t) * USND_CHANNELS);
    /* The wait made room for all of it; a short take means the source was
     * closed under us, and the rest is dropped rather than waited for. */
    usnd_push(pcm, frames);
    return true;
}

static Uint8 *SAUNAOSAUDIO_GetDeviceBuf(SDL_AudioDevice *device, int *buffer_size)
{
    (void)buffer_size;
    return device->hidden->mixbuf;
}

static void SAUNAOSAUDIO_CloseDevice(SDL_AudioDevice *device)
{
    if (!device->hidden) {
        return;
    }
    usnd_drain();         /* the last buffer is heard, not cut off */
    usnd_push_close();
    usnd_shutdown();
    SDL_free(device->hidden->mixbuf);
    SDL_free(device->hidden);
    device->hidden = NULL;
}

static bool SAUNAOSAUDIO_OpenDevice(SDL_AudioDevice *device)
{
    device->hidden = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*device->hidden));
    if (!device->hidden) {
        return false;
    }
    /* -ENODEV (no card) and -EBUSY (another program has it, no soundd) are
     * ordinary: the program opens no device and plays nothing, as on Linux
     * with nothing to connect to. */
    int err = usnd_init();
    if (err == 0) {
        err = usnd_push_open();
        if (err != 0) {
            usnd_shutdown();
        }
    }
    if (err != 0) {
        SDL_free(device->hidden);
        device->hidden = NULL;
        return SDL_SetError("SaunaOS: no sound (%s)", usnd_last_error());
    }
    device->spec.format = SDL_AUDIO_S16LE;
    device->spec.channels = USND_CHANNELS;
    device->spec.freq = USND_RATE;
    SDL_UpdatedAudioDeviceFormat(device);
    device->hidden->mixbuf = (Uint8 *)SDL_malloc(device->buffer_size);
    if (!device->hidden->mixbuf) {
        SAUNAOSAUDIO_CloseDevice(device);
        return false;
    }
    SDL_memset(device->hidden->mixbuf, device->silence_value, device->buffer_size);
    return true;
}

static bool SAUNAOSAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    impl->OpenDevice = SAUNAOSAUDIO_OpenDevice;
    impl->WaitDevice = SAUNAOSAUDIO_WaitDevice;
    impl->PlayDevice = SAUNAOSAUDIO_PlayDevice;
    impl->GetDeviceBuf = SAUNAOSAUDIO_GetDeviceBuf;
    impl->CloseDevice = SAUNAOSAUDIO_CloseDevice;
    impl->OnlyHasDefaultPlaybackDevice = true;
    impl->HasRecordingSupport = false;   /* no capture path in usnd yet */
    return true;
}

AudioBootStrap PRIVATEAUDIO_bootstrap = {
    "saunaos", "SaunaOS sound (usnd)", SAUNAOSAUDIO_Init, false, false
};

#endif /* SDL_AUDIO_DRIVER_PRIVATE */
