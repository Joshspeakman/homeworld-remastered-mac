/* Miles Sound System for the guest: the 25 AIL calls Aspyr's DirectSound layer (IDirectSound_Mac) makes, on our own
 * mixer. Samples play PCM straight from guest memory (8/16-bit, mono/stereo, any rate) with linear resampling and
 * per-channel volume, mixed in float to stereo at the output rate.
 *
 * A mixer thread renders blocks into a ring buffer that a CoreAudio output unit drains; the guest's sample callbacks
 * (start of buffer, end of buffer, end of sample) run on the mixer thread after the mix lock is released, so the
 * game's own locks never nest inside ours. HW_MUTE=1 (the default in test mode unless HW_SOUND=1) renders silence. */
#include "mac.h"
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <unistd.h>
#include <time.h>

enum { SMP_FREE = 1, SMP_DONE = 2, SMP_PLAYING = 4, SMP_STOPPED = 8 };
#define NSAMP 256
#define RATE 48000
#define BLOCK 256
typedef struct Smp {
    int used, status, fmt;                              /* fmt: bit 0 = 16-bit, bit 1 = stereo (DIG_F_*) */
    uint32_t addr, len, rate, loops;                    /* loops: 0 = forever */
    double pos;                                         /* in frames */
    float vl, vr;
    uint32_t cb_sob, cb_eob, cb_eos, user[8];
    int started;                                        /* SOB fired for the current pass */
} Smp;
static Smp smp[NSAMP];
static pthread_mutex_t mix_lock = PTHREAD_MUTEX_INITIALIZER;
static int muted;
#define RING (RATE / 4)                                 /* 250 ms of stereo frames */
static float ring[RING * 2]; static volatile uint32_t r_read, r_write;
static AudioUnit unit;

static OSStatus render(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io)
{
    (void)ref; (void)fl; (void)ts; (void)bus;
    float *out = io->mBuffers[0].mData;
    for (UInt32 k = 0; k < n; k++) {
        if (r_read != r_write) { uint32_t i = r_read % RING; out[2 * k] = ring[2 * i]; out[2 * k + 1] = ring[2 * i + 1]; r_read++; }
        else out[2 * k] = out[2 * k + 1] = 0;
    }
    return noErr;
}
typedef struct Ev { uint32_t fn, s; } Ev;
static void mix_block(float *buf, Ev *ev, int *nev)
{
    memset(buf, 0, sizeof(float) * 2 * BLOCK);
    pthread_mutex_lock(&mix_lock);
    for (int h = 1; h < NSAMP; h++) {
        Smp *s = &smp[h];
        if (!s->used || s->status != SMP_PLAYING || !s->addr || !s->len) continue;
        int b16 = s->fmt & 1, st = (s->fmt >> 1) & 1, fb = (b16 ? 2 : 1) * (st ? 2 : 1);
        double frames = (double)(s->len / (uint32_t)fb), step = (double)s->rate / RATE;
        if (!s->started) { s->started = 1; if (s->cb_sob && *nev < 64) ev[(*nev)++] = (Ev){s->cb_sob, (uint32_t)h}; }
        for (int k = 0; k < BLOCK; k++) {
            if (s->pos >= frames) {
                if (s->cb_eob && *nev < 64) ev[(*nev)++] = (Ev){s->cb_eob, (uint32_t)h};
                if (s->loops == 0 || s->loops > 1) { if (s->loops > 1) s->loops--; s->pos -= frames; s->started = 1;
                    if (s->cb_sob && *nev < 64) ev[(*nev)++] = (Ev){s->cb_sob, (uint32_t)h}; }
                else { s->status = SMP_DONE; if (s->cb_eos && *nev < 64) ev[(*nev)++] = (Ev){s->cb_eos, (uint32_t)h}; break; }
            }
            uint32_t i0 = (uint32_t)s->pos, i1 = i0 + 1 < (uint32_t)frames ? i0 + 1 : i0; float t = (float)(s->pos - i0);
            float l0, r0, l1, r1; const uint8_t *p = G_MEM + s->addr;
            if (b16) {
                const int16_t *q = (const int16_t *)p;
                l0 = q[i0 * (st + 1)] / 32768.f; r0 = st ? q[i0 * 2 + 1] / 32768.f : l0;
                l1 = q[i1 * (st + 1)] / 32768.f; r1 = st ? q[i1 * 2 + 1] / 32768.f : l1;
            } else {
                l0 = (p[i0 * (st + 1)] - 128) / 128.f; r0 = st ? (p[i0 * 2 + 1] - 128) / 128.f : l0;
                l1 = (p[i1 * (st + 1)] - 128) / 128.f; r1 = st ? (p[i1 * 2 + 1] - 128) / 128.f : l1;
            }
            buf[2 * k] += (l0 + (l1 - l0) * t) * s->vl; buf[2 * k + 1] += (r0 + (r1 - r0) * t) * s->vr;
            s->pos += step;
        }
    }
    pthread_mutex_unlock(&mix_lock);
}
static void *mixer(void *arg)
{
    (void)arg; pthread_setname_np("hw audio mixer");
    Ctx *c = mac_ctx(); float buf[2 * BLOCK]; Ev ev[64];
    for (;;) {
        while (r_write - r_read > RING - BLOCK - 1024) { struct timespec t = {0, 2000000}; nanosleep(&t, 0); }
        int nev = 0; mix_block(buf, ev, &nev);
        for (int k = 0; k < BLOCK; k++) {
            uint32_t i = r_write % RING;
            float l = buf[2 * k], r = buf[2 * k + 1];
            if (muted) l = r = 0;
            ring[2 * i] = l > 1 ? 1 : l < -1 ? -1 : l; ring[2 * i + 1] = r > 1 ? 1 : r < -1 ? -1 : r;
            r_write++;
        }
        for (int k = 0; k < nev; k++) g_call(c, ev[k].fn, 1, &ev[k].s);
    }
    return 0;
}
static int audio_start(void)
{
    static int started; if (started) return 1; started = 1;
    extern int hw_test_mode;
    muted = getenv("HW_MUTE") ? atoi(getenv("HW_MUTE")) : (hw_test_mode && !getenv("HW_SOUND"));
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(0, &d);
    if (!comp || AudioComponentInstanceNew(comp, &unit)) { hw_log("audio: no output unit"); return 0; }
    AudioStreamBasicDescription f = {RATE, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb = {render, 0};
    AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (AudioUnitInitialize(unit) || AudioOutputUnitStart(unit)) { hw_log("audio: output unit did not start"); return 0; }
    pthread_t t; pthread_create(&t, 0, mixer, 0); pthread_detach(t);
    hw_log("audio: %d Hz stereo%s", RATE, muted ? " (muted)" : "");
    return 1;
}

static Smp *S(uint32_t h) { return h && h < NSAMP && smp[h].used ? &smp[h] : 0; }
IMP(_AIL_startup) { RET(1); }
IMP(_AIL_set_redist_directory) { RET(ARG(0)); }
IMP(_AIL_get_preference) { RET(ARG(0) == 3 ? 64 : 0); }   /* DIG_MIXER_CHANNELS: the sample pool Aspyr preallocates */
IMP(_AIL_last_error) { static uint32_t g; if (!g) g = g_strdup(""); RET(g); }
IMP(_AIL_open_generic_digital_driver) { audio_start(); RET(0x4d494c53u & 0x0fffffffu); }   /* one driver */
IMP(_AIL_close_digital_driver) { RETV(); }
IMP(_AIL_allocate_sample_handle)
{
    pthread_mutex_lock(&mix_lock);
    for (uint32_t h = 1; h < NSAMP; h++) if (!smp[h].used) {
        memset(&smp[h], 0, sizeof smp[h]); smp[h].used = 1; smp[h].status = SMP_DONE; smp[h].vl = smp[h].vr = 1; smp[h].rate = 22050; smp[h].loops = 1;
        pthread_mutex_unlock(&mix_lock); RET(h);
    }
    pthread_mutex_unlock(&mix_lock); RET(0);
}
IMP(_AIL_release_sample_handle) { pthread_mutex_lock(&mix_lock); Smp *s = S(ARG(0)); if (s) s->used = 0; pthread_mutex_unlock(&mix_lock); RETV(); }
/* AIL_init_sample(S, format (DIG_F_*), flags) */
IMP(_AIL_init_sample)
{
    pthread_mutex_lock(&mix_lock); Smp *s = S(ARG(0));
    if (s) { s->fmt = (int)(ARG(1) & 3); s->status = SMP_DONE; s->pos = 0; s->loops = 1; s->vl = s->vr = 1; s->addr = s->len = 0; s->started = 0; }
    pthread_mutex_unlock(&mix_lock); RET(s ? 1 : 0);
}
IMP(_AIL_set_sample_address) { pthread_mutex_lock(&mix_lock); Smp *s = S(ARG(0)); if (s) { s->addr = ARG(1); s->len = ARG(2); s->pos = 0; s->started = 0; } pthread_mutex_unlock(&mix_lock); RET(1); }
IMP(_AIL_set_sample_playback_rate) { pthread_mutex_lock(&mix_lock); Smp *s = S(ARG(0)); if (s && ARG(1)) s->rate = ARG(1); pthread_mutex_unlock(&mix_lock); RETV(); }
IMP(_AIL_set_sample_volume_levels) { pthread_mutex_lock(&mix_lock); Smp *s = S(ARG(0)); if (s) { s->vl = ARGF(1); s->vr = ARGF(2); } pthread_mutex_unlock(&mix_lock); RETV(); }
IMP(_AIL_set_sample_loop_count) { pthread_mutex_lock(&mix_lock); Smp *s = S(ARG(0)); if (s) s->loops = ARG(1); pthread_mutex_unlock(&mix_lock); RETV(); }
IMP(_AIL_start_sample) { pthread_mutex_lock(&mix_lock); Smp *s = S(ARG(0)); if (s) { s->pos = 0; s->started = 0; s->status = SMP_PLAYING; } pthread_mutex_unlock(&mix_lock); RETV(); }
IMP(_AIL_stop_sample) { pthread_mutex_lock(&mix_lock); Smp *s = S(ARG(0)); if (s && s->status == SMP_PLAYING) s->status = SMP_STOPPED; pthread_mutex_unlock(&mix_lock); RETV(); }
IMP(_AIL_resume_sample) { pthread_mutex_lock(&mix_lock); Smp *s = S(ARG(0)); if (s && (s->status == SMP_STOPPED || s->status == SMP_DONE)) s->status = SMP_PLAYING; pthread_mutex_unlock(&mix_lock); RETV(); }
IMP(_AIL_end_sample) { pthread_mutex_lock(&mix_lock); Smp *s = S(ARG(0)); if (s) { s->status = SMP_DONE; s->pos = 0; } pthread_mutex_unlock(&mix_lock); RETV(); }
IMP(_AIL_sample_status) { Smp *s = S(ARG(0)); RET(s ? s->status : SMP_FREE); }
IMP(_AIL_sample_position)
{
    Smp *s = S(ARG(0)); if (!s) RET(0);
    if (getenv("HW_LOG_AIL")) { static int n; if (n++ % 500 == 0) hw_log("AIL sample %u: status %d pos %.0f/%u rate %u loops %u fmt %d", ARG(0), s->status, s->pos, s->len, s->rate, s->loops, s->fmt); }
    int fb = ((s->fmt & 1) ? 2 : 1) * ((s->fmt & 2) ? 2 : 1); RET((uint32_t)s->pos * (uint32_t)fb);
}
IMP(_AIL_set_sample_position)
{
    pthread_mutex_lock(&mix_lock); Smp *s = S(ARG(0));
    if (s) { int fb = ((s->fmt & 1) ? 2 : 1) * ((s->fmt & 2) ? 2 : 1); s->pos = (double)(ARG(1) / (uint32_t)fb); }
    pthread_mutex_unlock(&mix_lock); RETV();
}
IMP(_AIL_sample_user_data) { Smp *s = S(ARG(0)); RET(s && ARG(1) < 8 ? s->user[ARG(1)] : 0); }
IMP(_AIL_set_sample_user_data) { Smp *s = S(ARG(0)); if (s && ARG(1) < 8) s->user[ARG(1)] = ARG(2); RETV(); }
IMP(_AIL_register_SOB_callback) { Smp *s = S(ARG(0)); uint32_t o = s ? s->cb_sob : 0; if (s) s->cb_sob = ARG(1); RET(o); }
IMP(_AIL_register_EOB_callback) { Smp *s = S(ARG(0)); uint32_t o = s ? s->cb_eob : 0; if (s) s->cb_eob = ARG(1); RET(o); }
IMP(_AIL_register_EOS_callback) { Smp *s = S(ARG(0)); uint32_t o = s ? s->cb_eos : 0; if (s) s->cb_eos = ARG(1); RET(o); }
