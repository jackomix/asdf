/* opensles_audio.c -- OpenSL ES -> SDL2 audio bridge for the GDS loader
 * (0.79).  Unity 2022's FMOD backend (compiled into libunity.so) does not
 * use AudioTrack here; it dlopens "libOpenSLES.so" at runtime and dlsyms
 * slCreateEngine plus the SL_IID_* data symbols.  bionic.c fakes that
 * dlopen and routes those dlsyms to us via gds_opensles_sym().
 *
 * Design follows the reference ports' opensles_shim (horizonchase-nextos /
 * terraria-nextos, vendored under GDS_Unity/ref/): a vtable object model
 * (engine/outputmix/player + PLAY/VOLUME/BUFFERQUEUE interfaces), one 4MB
 * SPSC ring per player, and a dedicated 4ms pump thread that drains the
 * buffer-queue callbacks.  The pump thread is MANDATORY, not an
 * optimization: FMOD's System::init runs inside nativeRender and blocks
 * waiting for the first buffer-queue callback, so a pump driven from the
 * render thread deadlocks the boot.
 *
 * Audio out: SDL2, resolved by dlsym (no SDL headers in this build).
 * The loader already dlopens libSDL2 RTLD_GLOBAL for video, so the audio
 * API resolves from the same instance.  GDS_AUDIO=0 disables the shim
 * entirely (dlsym then yields NULL and FMOD skips OpenSL -> silence,
 * same behavior as 0.78).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>

typedef uint32_t SLresult;
typedef uint32_t SLuint32;
typedef int32_t  SLint32;
typedef uint32_t SLmillibel;
typedef uint32_t SLmillisecond;
typedef uint32_t SLBoolean;
typedef const void *SLInterfaceID;

#define SL_RESULT_SUCCESS              ((SLresult)0)
#define SL_RESULT_RESOURCE_ERROR       ((SLresult)0x0000000D)
#define SL_RESULT_FEATURE_UNSUPPORTED  ((SLresult)12)
#define SL_BOOLEAN_FALSE               ((SLBoolean)0)
#define SL_BOOLEAN_TRUE                ((SLBoolean)1)
#define SL_PLAYSTATE_STOPPED           ((SLuint32)1)
#define SL_PLAYSTATE_PLAYING           ((SLuint32)3)
#define SL_TIME_UNKNOWN                ((SLmillisecond)0xFFFFFFFF)
#define SL_PLAYEVENT_HEADATEND         ((SLuint32)0x00000001)
#define SL_DATAFORMAT_PCM              2
#define SL_DATALOCATOR_BUFFERQUEUE     0x800007BD
#define SL_OBJECT_STATE_REALIZED       2

/* ---- interface identity tags (values must be stable, unique addresses) */
static const int tag_engine = 1;
static const int tag_play = 2;
static const int tag_volume = 3;
static const int tag_bufferqueue = 4;
static const int tag_effectsend = 5;
static const int tag_enginecap = 6;
static const int tag_envreverb = 7;
static const int tag_androidcfg = 8;
static const int tag_record = 9;
static const int tag_asbq = 10;   /* SL_IID_ANDROIDSIMPLEBUFFERQUEUE */

/* dlsym("SL_IID_*") must hand out the ADDRESS of a global whose VALUE is
 * the interface pointer, exactly like the real libOpenSLES exports them. */
static const SLInterfaceID v_IID_ENGINE = &tag_engine;
static const SLInterfaceID v_IID_PLAY = &tag_play;
static const SLInterfaceID v_IID_VOLUME = &tag_volume;
static const SLInterfaceID v_IID_BUFFERQUEUE = &tag_bufferqueue;
static const SLInterfaceID v_IID_EFFECTSEND = &tag_effectsend;
static const SLInterfaceID v_IID_ENGINECAPABILITIES = &tag_enginecap;
static const SLInterfaceID v_IID_ENVIRONMENTALREVERB = &tag_envreverb;
static const SLInterfaceID v_IID_ANDROIDCONFIGURATION = &tag_androidcfg;
static const SLInterfaceID v_IID_RECORD = &tag_record;
static const SLInterfaceID v_IID_ANDROIDSIMPLEBUFFERQUEUE = &tag_asbq;

#define MAX_PLAYERS 16
#define RING_BYTES  (4 * 1024 * 1024)
#define RING_MASK   (RING_BYTES - 1)
#define SDL_AUDIO_SAMPLES 2048
#define SDL_OUTPUT_RATE   44100
#define TMP_SAMPLES (SDL_AUDIO_SAMPLES * 2)

/* ---------------------------------------------------------------- SDL */
/* Minimal SDL2 audio surface, dlsym'd: no SDL headers in this build. */
#define SDL_INIT_AUDIO 0x10u
#define CUR_AUDIO_S16SYS 0x8010u            /* AUDIO_S16LSB, aarch64 LE */
#define SDL_AUDIO_ALLOW_SAMPLES_CHANGE 0x4u

typedef void (*sdl_audio_cb_t)(void *userdata, uint8_t *stream, int len);
typedef struct {
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    sdl_audio_cb_t callback;
    void *userdata;
} sdl_spec_t;

static uint32_t (*p_SDL_InitSubSystem)(uint32_t);
static uint32_t (*p_SDL_WasInit)(uint32_t);
static const char *(*p_SDL_GetError)(void);
static uint32_t (*p_SDL_OpenAudioDevice)(const char *, int, const sdl_spec_t *,
                                         sdl_spec_t *, int);
static const char *(*p_SDL_GetCurrentAudioDriver)(void);
static void (*p_SDL_PauseAudioDevice)(uint32_t, int);
static void (*p_SDL_LockAudioDevice)(uint32_t);
static void (*p_SDL_UnlockAudioDevice)(uint32_t);
static void (*p_SDL_CloseAudioDevice)(uint32_t);
static int g_sdl_audio_ok = 0;      /* resolution state: 1 = usable */
static int g_sdl_audio_tried = 0;

static int sdl_audio_resolve(void) {
    if (g_sdl_audio_tried) return g_sdl_audio_ok;
    g_sdl_audio_tried = 1;
    void *h = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("libSDL2.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) {
        fprintf(stderr, "[SL] SDL2 handle unavailable -- audio off\n");
        return 0;
    }
#define R(f) p_##f = (void *)dlsym(h, #f)
    R(SDL_InitSubSystem); R(SDL_WasInit); R(SDL_GetError);
    R(SDL_OpenAudioDevice); R(SDL_GetCurrentAudioDriver);
    R(SDL_PauseAudioDevice); R(SDL_LockAudioDevice); R(SDL_UnlockAudioDevice);
    R(SDL_CloseAudioDevice);
#undef R
    if (!p_SDL_OpenAudioDevice || !p_SDL_PauseAudioDevice ||
        !p_SDL_LockAudioDevice || !p_SDL_UnlockAudioDevice) {
        fprintf(stderr, "[SL] SDL audio entry points missing -- audio off\n");
        return 0;
    }
    g_sdl_audio_ok = 1;
    return 1;
}

/* ------------------------------------------------------- object model */
typedef struct {
    SLuint32 locatorType;
    SLuint32 numBuffers;
} SLDataLocator_BufferQueue;

typedef struct {
    SLuint32 formatType;
    SLuint32 numChannels;
    SLuint32 samplesPerSec;      /* milliHz */
    SLuint32 bitsPerSample;
    SLuint32 containerSize;
    SLuint32 channelMask;
    SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct { void *pLocator; void *pFormat; } SLDataSource;
typedef struct { void *pLocator; void *pFormat; } SLDataSink;

typedef void (*bq_cb_t)(void *caller, void *pContext);

typedef struct {
    uint8_t ring[RING_BYTES];
    volatile uint32_t ring_head;
    volatile uint32_t ring_tail;

    uint32_t queued_sizes[64];
    volatile uint32_t queued_head_index;
    volatile uint32_t queued_tail_index;
    volatile uint32_t queued_count;
    volatile uint32_t queued_front_offset;
    uint32_t queue_capacity;

    bq_cb_t callback;
    void *callback_context;
    uint32_t last_enqueue_size;
    uint32_t enqueue_counter;

    void (*play_callback)(void *caller, void *pContext, SLuint32 event);
    void *play_callback_context;
    SLuint32 play_event_mask;

    int ever_enqueued;
    int headatend_fired;
    int decoder_done;
    volatile uint32_t frames_played;

    volatile SLuint32 play_state;
    float volume;
    int active;
    uint64_t played_bytes;

    SLuint32 num_channels;
    SLuint32 sample_rate;
    SLuint32 bits_per_sample;

    void *obj_vtable[8];
    void *obj_ptr;
    void *play_vtable[12];
    void *play_ptr;
    void *volume_vtable[8];
    void *volume_ptr;
    void *bq_vtable[8];
    void *bq_ptr;
    void *effectsend_vtable[8];
    void *effectsend_ptr;
} AudioPlayer;

static AudioPlayer g_players[MAX_PLAYERS];
static pthread_mutex_t g_players_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_audio_dev = 0;
static int g_audio_initialized = 0;
static volatile int g_pump_thread_on = 0;

/* ------------------------------------------------------ ring helpers */
static void queue_reset(AudioPlayer *p) {
    memset(p->queued_sizes, 0, sizeof p->queued_sizes);
    p->queued_head_index = 0;
    p->queued_tail_index = 0;
    p->queued_count = 0;
    p->queued_front_offset = 0;
    p->played_bytes = 0;
}

static void queue_push(AudioPlayer *p, uint32_t size) {
    if (!size) return;
    if (p->queued_count >= 64) {
        uint32_t tail = (p->queued_tail_index - 1) % 64;
        p->queued_sizes[tail] += size;
        return;
    }
    p->queued_sizes[p->queued_tail_index % 64] = size;
    p->queued_tail_index++;
    p->queued_count++;
}

static void queue_consume(AudioPlayer *p, uint32_t bytes) {
    while (bytes && p->queued_count) {
        uint32_t head = p->queued_head_index % 64;
        uint32_t remaining = p->queued_sizes[head] - p->queued_front_offset;
        if (bytes < remaining) { p->queued_front_offset += bytes; return; }
        bytes -= remaining;
        p->queued_sizes[head] = 0;
        p->queued_head_index++;
        p->queued_count--;
        p->queued_front_offset = 0;
    }
}

static inline uint32_t ring_readable(const AudioPlayer *p) {
    return p->ring_head - p->ring_tail;
}
static inline uint32_t ring_writable(const AudioPlayer *p) {
    return RING_BYTES - (p->ring_head - p->ring_tail);
}
static uint32_t ring_write(AudioPlayer *p, const void *data, uint32_t len) {
    uint32_t space = ring_writable(p);
    if (len > space) len = space;
    if (!len) return 0;
    const uint8_t *src = data;
    uint32_t head = p->ring_head & RING_MASK;
    uint32_t first = RING_BYTES - head;
    if (first > len) first = len;
    memcpy(p->ring + head, src, first);
    if (len > first) memcpy(p->ring, src + first, len - first);
    __sync_synchronize();
    p->ring_head += len;
    return len;
}
static uint32_t ring_read(AudioPlayer *p, void *data, uint32_t len) {
    uint32_t avail = ring_readable(p);
    if (len > avail) len = avail;
    if (!len) return 0;
    uint8_t *dst = data;
    uint32_t tail = p->ring_tail & RING_MASK;
    uint32_t first = RING_BYTES - tail;
    if (first > len) first = len;
    memcpy(dst, p->ring + tail, first);
    if (len > first) memcpy(dst + first, p->ring, len - first);
    __sync_synchronize();
    p->ring_tail += len;
    return len;
}

/* ------------------------------------------------------- SDL mixing */
static void sdl_audio_callback(void *userdata, uint8_t *stream, int len) {
    (void)userdata;
    memset(stream, 0, (size_t)len);
    int out_samples = len / 2;             /* int16 */
    int16_t *out = (int16_t *)stream;
    if (out_samples > TMP_SAMPLES) out_samples = TMP_SAMPLES;

    static float mix_buf[TMP_SAMPLES];
    memset(mix_buf, 0, (size_t)out_samples * sizeof(float));
    uint32_t out_frames = (uint32_t)out_samples / 2;
    int16_t tmp[TMP_SAMPLES];

    for (int i = 0; i < MAX_PLAYERS; i++) {
        AudioPlayer *p = &g_players[i];
        if (!p->active || p->play_state != SL_PLAYSTATE_PLAYING) continue;

        uint32_t src_rate = p->sample_rate ? p->sample_rate : SDL_OUTPUT_RATE;
        uint32_t src_channels = p->num_channels ? p->num_channels : 2;
        uint32_t frame_size = src_channels * sizeof(int16_t);
        float vol = p->volume;
        if (!(vol >= 0.0f && vol <= 2.0f)) vol = 0.0f;   /* corrupt: mute */
        vol *= (src_channels == 1) ? 0.35f : 0.8f;

        uint32_t src_frames_needed = (src_rate == SDL_OUTPUT_RATE)
            ? out_frames
            : (uint32_t)((uint64_t)out_frames * src_rate / SDL_OUTPUT_RATE) + 2;
        uint32_t src_bytes = src_frames_needed * frame_size;
        if (src_bytes > sizeof tmp) src_bytes = sizeof tmp;
        src_bytes = (src_bytes / frame_size) * frame_size;

        uint32_t got = ring_read(p, tmp, src_bytes);
        got = (got / frame_size) * frame_size;
        uint32_t src_frames = got / frame_size;
        if (!src_frames) continue;
        queue_consume(p, got);
        p->played_bytes += got;

        /* underrun: fade the tail so the hole doesn't click */
        int underrun = (got < src_bytes);
        uint32_t fade_len = 64, fade_start = 0;
        if (underrun) {
            if (src_frames > fade_len) fade_start = src_frames - fade_len;
            else { fade_start = 0; fade_len = src_frames; }
        }
        /* fade-in over the player's first 32 frames */
        uint32_t fadein_left = (p->frames_played < 32)
                               ? (32 - p->frames_played) : 0;

        if (src_rate == SDL_OUTPUT_RATE && src_channels == 2) {
            uint32_t n = src_frames > out_frames ? out_frames : src_frames;
            for (uint32_t f = 0; f < n; f++) {
                float env = 1.0f;
                if (f < fadein_left)
                    env = (float)(p->frames_played + f) / 32.0f;
                if (underrun && f >= fade_start && fade_len) {
                    float fo = 1.0f - (float)(f - fade_start) / (float)fade_len;
                    if (fo < env) env = fo;
                }
                if (env < 0.0f) env = 0.0f;
                float v = vol * env;
                mix_buf[f * 2]     += (float)tmp[f * 2] * v;
                mix_buf[f * 2 + 1] += (float)tmp[f * 2 + 1] * v;
            }
            p->frames_played += n;
        } else {
            uint32_t step = (uint32_t)((uint64_t)src_rate * 65536 / SDL_OUTPUT_RATE);
            uint32_t pos = 0;
            uint32_t fade_start_out = underrun
                ? (uint32_t)((uint64_t)fade_start * SDL_OUTPUT_RATE / src_rate)
                : out_frames + 1;
            uint32_t fade_len_out =
                (uint32_t)((uint64_t)fade_len * SDL_OUTPUT_RATE / src_rate);
            if (!fade_len_out) fade_len_out = 1;
            uint32_t mixed = 0;
            for (uint32_t f = 0; f < out_frames; f++) {
                uint32_t idx = pos >> 16;
                uint32_t frac = pos & 0xFFFF;
                if (idx >= src_frames) break;
                float l0, r0, l1, r1;
                if (src_channels == 1) {
                    l0 = r0 = (float)tmp[idx];
                    l1 = r1 = (idx + 1 < src_frames) ? (float)tmp[idx + 1] : l0;
                } else {
                    l0 = (float)tmp[idx * 2];
                    r0 = (float)tmp[idx * 2 + 1];
                    if (idx + 1 < src_frames) {
                        l1 = (float)tmp[(idx + 1) * 2];
                        r1 = (float)tmp[(idx + 1) * 2 + 1];
                    } else { l1 = l0; r1 = r0; }
                }
                float t = (float)frac / 65536.0f;
                float left = l0 + (l1 - l0) * t;
                float right = r0 + (r1 - r0) * t;
                float env = 1.0f;
                if (f < fadein_left)
                    env = (float)(p->frames_played + f) / 32.0f;
                if (f >= fade_start_out && fade_len_out) {
                    float fo = 1.0f -
                        (float)(f - fade_start_out) / (float)fade_len_out;
                    if (fo < env) env = fo;
                }
                if (env < 0.0f) env = 0.0f;
                float v = vol * env;
                mix_buf[f * 2]     += left * v;
                mix_buf[f * 2 + 1] += right * v;
                pos += step;
                mixed++;
            }
            p->frames_played += mixed;
        }
    }

    /* soft limiter (smooth knee, no discontinuities) */
    const float master_gain = 0.30f;
    const float threshold = 28000.0f;
    const float knee = 4000.0f;
    for (int s = 0; s < out_samples; s++) {
        float x = mix_buf[s] * master_gain;
        float ax = fabsf(x);
        if (ax > threshold) {
            float over = ax - threshold;
            float compressed = threshold + knee * (over / (over + knee));
            x = (x > 0) ? compressed : -compressed;
        }
        if (x > 32767.0f) x = 32767.0f;
        if (x < -32768.0f) x = -32768.0f;
        out[s] = (int16_t)x;
    }
}

/* ------------------------------------------------ pump (own thread!) */
static void pump_callbacks_impl(void);

static void *sl_pump_thread(void *arg) {
    (void)arg;
    fprintf(stderr, "[SL] pump thread up (4ms)\n");
    for (;;) { pump_callbacks_impl(); usleep(4000); }
    return NULL;
}

static void ensure_audio_initialized(void) {
    if (g_audio_initialized) return;
    g_audio_initialized = 1;    /* one-shot even on failure */
    if (!sdl_audio_resolve()) return;

    sdl_spec_t want, have;
    memset(&want, 0, sizeof want);
    want.freq = SDL_OUTPUT_RATE;
    want.format = CUR_AUDIO_S16SYS;
    want.channels = 2;
    want.samples = SDL_AUDIO_SAMPLES;
    want.callback = sdl_audio_callback;

    if (p_SDL_WasInit && !p_SDL_WasInit(SDL_INIT_AUDIO) && p_SDL_InitSubSystem)
        if (p_SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
            fprintf(stderr, "[SL] SDL_InitSubSystem(AUDIO): %s\n",
                    p_SDL_GetError ? p_SDL_GetError() : "?");
    g_audio_dev = p_SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                        SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (!g_audio_dev) {
        fprintf(stderr, "[SL] SDL_OpenAudioDevice FAILED: %s (driver=%s)\n",
                p_SDL_GetError ? p_SDL_GetError() : "?",
                (p_SDL_GetCurrentAudioDriver && p_SDL_GetCurrentAudioDriver())
                    ? p_SDL_GetCurrentAudioDriver() : "?");
        return;
    }
    fprintf(stderr, "[SL] SDL audio open: %dHz %dch %d samples (driver=%s)\n",
            have.freq, have.channels, have.samples,
            (p_SDL_GetCurrentAudioDriver && p_SDL_GetCurrentAudioDriver())
                ? p_SDL_GetCurrentAudioDriver() : "?");
    p_SDL_PauseAudioDevice(g_audio_dev, 0);
    if (!g_pump_thread_on) {
        g_pump_thread_on = 1;
        pthread_t pt;
        pthread_create(&pt, NULL, sl_pump_thread, NULL);
        pthread_detach(pt);
    }
}

/* --------------------------------------------- player slot management */
static void player_reset_meta(AudioPlayer *p) {
    p->ring_head = 0;
    p->ring_tail = 0;
    queue_reset(p);
    p->queue_capacity = 0;
    p->callback = NULL;
    p->callback_context = NULL;
    p->last_enqueue_size = 0;
    p->enqueue_counter = 0;
    p->play_callback = NULL;
    p->play_callback_context = NULL;
    p->play_event_mask = 0;
    p->ever_enqueued = 0;
    p->headatend_fired = 0;
    p->decoder_done = 0;
    p->frames_played = 0;
    p->play_state = SL_PLAYSTATE_STOPPED;
    p->volume = 1.0f;
    p->active = 1;
    p->num_channels = 0;
    p->sample_rate = 0;
    p->bits_per_sample = 0;
}

static AudioPlayer *alloc_player(void) {
    pthread_mutex_lock(&g_players_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!g_players[i].active) {
            player_reset_meta(&g_players[i]);
            pthread_mutex_unlock(&g_players_lock);
            return &g_players[i];
        }
    }
    /* recycle a stopped slot */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        AudioPlayer *p = &g_players[i];
        if (p->play_state == SL_PLAYSTATE_STOPPED) {
            if (g_audio_dev && p_SDL_LockAudioDevice)
                p_SDL_LockAudioDevice(g_audio_dev);
            player_reset_meta(p);
            if (g_audio_dev && p_SDL_UnlockAudioDevice)
                p_SDL_UnlockAudioDevice(g_audio_dev);
            pthread_mutex_unlock(&g_players_lock);
            return p;
        }
    }
    /* last resort: kill the oldest */
    int oldest = -1;
    uint64_t most = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_players[i].played_bytes >= most) {
            most = g_players[i].played_bytes;
            oldest = i;
        }
    }
    if (oldest >= 0) {
        fprintf(stderr, "[SL] WARNING: force-killing player %d\n", oldest);
        if (g_audio_dev && p_SDL_LockAudioDevice)
            p_SDL_LockAudioDevice(g_audio_dev);
        g_players[oldest].play_state = SL_PLAYSTATE_STOPPED;
        player_reset_meta(&g_players[oldest]);
        if (g_audio_dev && p_SDL_UnlockAudioDevice)
            p_SDL_UnlockAudioDevice(g_audio_dev);
        pthread_mutex_unlock(&g_players_lock);
        return &g_players[oldest];
    }
    pthread_mutex_unlock(&g_players_lock);
    fprintf(stderr, "[SL] FATAL: no player slots\n");
    return NULL;
}

/* mapping from interface pointer back to the owning player slot */
static AudioPlayer *owner_of(void *self, size_t field /*offsetof ptr field*/) {
    void **itf = (void **)self;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if ((void **)((char *)&g_players[i] + field) == itf)
            return &g_players[i];
    }
    return NULL;
}
#define OFF_PLAYPTR  offsetof(AudioPlayer, play_ptr)
#define OFF_VOLPTR   offsetof(AudioPlayer, volume_ptr)
#define OFF_OBJPTR   offsetof(AudioPlayer, obj_ptr)

/* ------------------------------------------------------- SLPlayItf */
static SLresult play_SetPlayState(void *self, SLuint32 state) {
    AudioPlayer *p = owner_of(self, OFF_PLAYPTR);
    if (!p) return SL_RESULT_SUCCESS;
    { static int n; if (n++ < 12)
        fprintf(stderr, "[SL] SetPlayState %u->%u\n", p->play_state, state); }
    if (g_audio_dev && p_SDL_LockAudioDevice) p_SDL_LockAudioDevice(g_audio_dev);
    if (state == SL_PLAYSTATE_STOPPED && p->play_state != SL_PLAYSTATE_STOPPED) {
        p->headatend_fired = 0;
        p->decoder_done = 0;
        p->ring_head = p->ring_tail = 0;
        queue_reset(p);
    }
    if (state == SL_PLAYSTATE_PLAYING && p->play_state != SL_PLAYSTATE_PLAYING)
        p->frames_played = 0;
    p->play_state = state;
    if (g_audio_dev && p_SDL_UnlockAudioDevice)
        p_SDL_UnlockAudioDevice(g_audio_dev);
    return SL_RESULT_SUCCESS;
}

static SLresult play_GetPlayState(void *self, SLuint32 *pState) {
    AudioPlayer *p = owner_of(self, OFF_PLAYPTR);
    if (pState) *pState = p ? p->play_state : SL_PLAYSTATE_STOPPED;
    return SL_RESULT_SUCCESS;
}

static SLresult play_GetDuration(void *self, SLmillisecond *pMsec) {
    (void)self;
    if (pMsec) *pMsec = SL_TIME_UNKNOWN;
    return SL_RESULT_SUCCESS;
}

static SLresult play_GetPosition(void *self, SLmillisecond *pMsec) {
    AudioPlayer *p = owner_of(self, OFF_PLAYPTR);
    if (!p) { if (pMsec) *pMsec = 0; return SL_RESULT_SUCCESS; }
    uint64_t pos = 0;
    uint32_t ch = p->num_channels ? p->num_channels : 2;
    uint32_t rate = p->sample_rate ? p->sample_rate : SDL_OUTPUT_RATE;
    uint32_t bpf = ch * sizeof(int16_t);
    if (bpf && rate) pos = (p->played_bytes / bpf) * 1000ULL / rate;
    if (pMsec) *pMsec = (SLmillisecond)pos;
    return SL_RESULT_SUCCESS;
}

static SLresult play_RegisterCallback(void *self, void *cb, void *ctx) {
    AudioPlayer *p = owner_of(self, OFF_PLAYPTR);
    if (!p) return SL_RESULT_SUCCESS;
    { static int n; if (n++ < 4)
        fprintf(stderr, "[SL] play_RegisterCallback cb=%p\n", cb); }
    p->play_callback = (void (*)(void *, void *, SLuint32))cb;
    p->play_callback_context = ctx;
    return SL_RESULT_SUCCESS;
}

static SLresult play_SetCallbackEventsMask(void *self, SLuint32 mask) {
    AudioPlayer *p = owner_of(self, OFF_PLAYPTR);
    if (p) p->play_event_mask = mask;
    return SL_RESULT_SUCCESS;
}

/* ----------------------------------------------------- SLVolumeItf */
static SLresult volume_SetVolumeLevel(void *self, SLmillibel level) {
    AudioPlayer *p = owner_of(self, OFF_VOLPTR);
    float linear = (level <= -9600) ? 0.0f : powf(10.0f, level / 2000.0f);
    if (linear > 2.0f) linear = 1.0f;      /* clamp insanity */
    if (p) p->volume = linear;
    return SL_RESULT_SUCCESS;
}

static SLresult volume_GetVolumeLevel(void *self, SLmillibel *pLevel) {
    (void)self;
    if (pLevel) *pLevel = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult volume_GetMaxVolumeLevel(void *self, SLmillibel *pMax) {
    (void)self;
    if (pMax) *pMax = 0;
    return SL_RESULT_SUCCESS;
}

/* ------------------------------------------------- SLBufferQueueItf */
static SLresult bq_Enqueue(void *self, const void *buf, SLuint32 size) {
    AudioPlayer *p = owner_of(self, offsetof(AudioPlayer, bq_ptr));
    { static int n; if (n++ < 16)
        fprintf(stderr, "[SL] bq_Enqueue #%d size=%u (PCM flowing)\n",
                n, (unsigned)size); }
    if (!p) return SL_RESULT_SUCCESS;
    uint32_t written = ring_write(p, buf, size);
    if (written != size)
        fprintf(stderr, "[SL] WARNING: truncated enqueue (%u/%u)\n",
                written, size);
    queue_push(p, written);
    if (written) {
        p->last_enqueue_size = written;
        p->enqueue_counter++;
        p->ever_enqueued = 1;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self) {
    AudioPlayer *p = owner_of(self, offsetof(AudioPlayer, bq_ptr));
    if (!p) return SL_RESULT_SUCCESS;
    if (g_audio_dev && p_SDL_LockAudioDevice) p_SDL_LockAudioDevice(g_audio_dev);
    p->ring_head = p->ring_tail = 0;
    queue_reset(p);
    p->enqueue_counter = 0;
    p->ever_enqueued = 0;
    p->headatend_fired = 0;
    p->decoder_done = 0;
    if (g_audio_dev && p_SDL_UnlockAudioDevice)
        p_SDL_UnlockAudioDevice(g_audio_dev);
    return SL_RESULT_SUCCESS;
}

static SLresult bq_GetState(void *self, void *pState) {
    AudioPlayer *p = owner_of(self, offsetof(AudioPlayer, bq_ptr));
    if (p && pState) {
        SLuint32 *st = pState;
        uint32_t capacity = p->queue_capacity ? p->queue_capacity : 1;
        st[0] = p->queued_count;
        st[1] = p->queued_count ? p->queued_head_index % capacity : 0;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(void *self, bq_cb_t cb, void *ctx) {
    AudioPlayer *p = owner_of(self, offsetof(AudioPlayer, bq_ptr));
    if (!p) return SL_RESULT_SUCCESS;
    { static int n; if (n++ < 4)
        fprintf(stderr, "[SL] bq_RegisterCallback cb=%p\n", (void *)cb); }
    p->callback = cb;
    p->callback_context = ctx;
    return SL_RESULT_SUCCESS;
}

/* ------------------------------------------------ misc object stubs */
static SLresult stub_success(void) { return SL_RESULT_SUCCESS; }
static SLresult engine_Unsupported(void) {
    /* FMOD also probes for a recorder; FEATURE_UNSUPPORTED makes it drop
     * that subsystem cleanly instead of using a garbage object. */
    static int n;
    if (n++ < 6)
        fprintf(stderr, "[SL] unsupported engine op -> FEATURE_UNSUPPORTED\n");
    return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult obj_GetState(void *self, SLuint32 *pState) {
    /* FMOD checks state after Realize; a passive stub never writes *pState
     * and its uninitialized garbage used to abort the init. */
    (void)self;
    if (pState) *pState = SL_OBJECT_STATE_REALIZED;
    return SL_RESULT_SUCCESS;
}

static SLresult player_Realize(void *self, SLBoolean async) {
    (void)self; (void)async;
    return SL_RESULT_SUCCESS;
}

static SLresult player_GetInterface(void *self, SLInterfaceID iid, void **out) {
    AudioPlayer *p = owner_of(self, OFF_OBJPTR);
    if (!p) return SL_RESULT_SUCCESS;
    if (iid == v_IID_PLAY) *out = &p->play_ptr;
    else if (iid == v_IID_VOLUME) *out = &p->volume_ptr;
    else if (iid == v_IID_BUFFERQUEUE || iid == v_IID_ANDROIDSIMPLEBUFFERQUEUE)
        *out = &p->bq_ptr;
    else if (iid == v_IID_ANDROIDCONFIGURATION) *out = &p->effectsend_ptr;
    else if (iid == v_IID_EFFECTSEND) *out = &p->effectsend_ptr;
    else {
        static int n;
        if (n++ < 8)
            fprintf(stderr, "[SL] player GetInterface(iid=%p unknown)\n",
                    (void *)iid);
        *out = &p->effectsend_ptr;
    }
    return SL_RESULT_SUCCESS;
}

static void player_Destroy(void *self) {
    AudioPlayer *p = owner_of(self, OFF_OBJPTR);
    if (!p) return;
    if (g_audio_dev && p_SDL_LockAudioDevice) p_SDL_LockAudioDevice(g_audio_dev);
    p->play_state = SL_PLAYSTATE_STOPPED;
    p->active = 0;
    if (g_audio_dev && p_SDL_UnlockAudioDevice)
        p_SDL_UnlockAudioDevice(g_audio_dev);
}

static void setup_player_vtables(AudioPlayer *p) {
    for (int i = 0; i < 8; i++) p->obj_vtable[i] = (void *)stub_success;
    p->obj_vtable[0] = (void *)player_Realize;
    p->obj_vtable[2] = (void *)obj_GetState;
    p->obj_vtable[3] = (void *)player_GetInterface;
    p->obj_vtable[6] = (void *)player_Destroy;
    p->obj_ptr = p->obj_vtable;

    for (int i = 0; i < 12; i++) p->play_vtable[i] = (void *)stub_success;
    p->play_vtable[0] = (void *)play_SetPlayState;
    p->play_vtable[1] = (void *)play_GetPlayState;
    p->play_vtable[2] = (void *)play_GetDuration;
    p->play_vtable[3] = (void *)play_GetPosition;
    p->play_vtable[4] = (void *)play_RegisterCallback;
    p->play_vtable[5] = (void *)play_SetCallbackEventsMask;
    p->play_ptr = p->play_vtable;

    for (int i = 0; i < 8; i++) p->volume_vtable[i] = (void *)stub_success;
    p->volume_vtable[0] = (void *)volume_SetVolumeLevel;
    p->volume_vtable[1] = (void *)volume_GetVolumeLevel;
    p->volume_vtable[2] = (void *)volume_GetMaxVolumeLevel;
    p->volume_ptr = p->volume_vtable;

    for (int i = 0; i < 8; i++) p->bq_vtable[i] = (void *)stub_success;
    p->bq_vtable[0] = (void *)bq_Enqueue;
    p->bq_vtable[1] = (void *)bq_Clear;
    p->bq_vtable[2] = (void *)bq_GetState;
    p->bq_vtable[3] = (void *)bq_RegisterCallback;
    p->bq_ptr = p->bq_vtable;

    for (int i = 0; i < 8; i++) p->effectsend_vtable[i] = (void *)stub_success;
    p->effectsend_ptr = p->effectsend_vtable;
}

/* -------------------------------------------------------- output mix */
static void *g_outmix_vtable[8];
static void *g_outmix_ptr;

static SLresult outmix_Realize(void *self, SLBoolean async) {
    (void)self; (void)async;
    return SL_RESULT_SUCCESS;
}

static SLresult outmix_GetInterface(void *self, SLInterfaceID iid, void **out) {
    (void)self; (void)iid;
    static void *stub_vt[8];
    static void *stub_ptr;
    static int inited = 0;
    if (!inited) {
        for (int i = 0; i < 8; i++) stub_vt[i] = (void *)stub_success;
        stub_ptr = stub_vt;
        inited = 1;
    }
    if (out) *out = &stub_ptr;
    return SL_RESULT_SUCCESS;
}

static void outmix_Destroy(void *self) { (void)self; }

static void init_outmix(void) {
    static int inited = 0;
    if (inited) return;
    inited = 1;
    for (int i = 0; i < 8; i++) g_outmix_vtable[i] = (void *)stub_success;
    g_outmix_vtable[0] = (void *)outmix_Realize;
    g_outmix_vtable[2] = (void *)obj_GetState;
    g_outmix_vtable[3] = (void *)outmix_GetInterface;
    g_outmix_vtable[6] = (void *)outmix_Destroy;
    g_outmix_ptr = g_outmix_vtable;
}

/* ------------------------------------------------------------ engine */
static void *g_eng_obj_vt[8];
static void *g_eng_obj_ptr;
static void *g_eng_itf_vt[16];
static void *g_eng_itf_ptr;

static SLresult engine_CreateOutputMix(void *self, void **pMix,
                                       SLuint32 numInterfaces,
                                       const SLInterfaceID *ids,
                                       const SLBoolean *required) {
    (void)self; (void)numInterfaces; (void)ids; (void)required;
    fprintf(stderr, "[SL] CreateOutputMix\n");
    init_outmix();
    if (pMix) *pMix = &g_outmix_ptr;
    return SL_RESULT_SUCCESS;
}

static SLresult engine_CreateAudioPlayer(void *self, void **pPlayer,
                                         void *pAudioSrc, void *pAudioSnk,
                                         SLuint32 numInterfaces,
                                         const SLInterfaceID *ids,
                                         const SLBoolean *required) {
    (void)self; (void)pAudioSnk; (void)numInterfaces; (void)ids; (void)required;
    fprintf(stderr, "[SL] CreateAudioPlayer\n");
    ensure_audio_initialized();

    AudioPlayer *p = alloc_player();
    if (!p) {
        if (pPlayer) *pPlayer = NULL;
        return SL_RESULT_RESOURCE_ERROR;
    }

    if (pAudioSrc) {
        SLDataSource *src = pAudioSrc;
        if (src->pLocator) {
            SLDataLocator_BufferQueue *loc = src->pLocator;
            if (loc->locatorType == SL_DATALOCATOR_BUFFERQUEUE)
                p->queue_capacity = loc->numBuffers;
        }
        if (src->pFormat) {
            SLDataFormat_PCM *fmt = src->pFormat;
            if (fmt->formatType == SL_DATAFORMAT_PCM) {
                p->num_channels = fmt->numChannels;
                p->sample_rate = fmt->samplesPerSec / 1000;   /* milliHz */
                p->bits_per_sample = fmt->bitsPerSample;
                fprintf(stderr, "[SL] player fmt: %uch %uHz %ubit (q=%u)\n",
                        p->num_channels, p->sample_rate,
                        p->bits_per_sample, p->queue_capacity);
            } else {
                fprintf(stderr, "[SL] player formatType=0x%x NOT plain PCM\n",
                        (unsigned)fmt->formatType);
            }
        }
    }
    if (!p->sample_rate) {
        p->num_channels = 2;
        p->sample_rate = SDL_OUTPUT_RATE;
        p->bits_per_sample = 16;
    }
    setup_player_vtables(p);
    if (pPlayer) *pPlayer = &p->obj_ptr;
    return SL_RESULT_SUCCESS;
}

static SLresult engine_obj_Realize(void *self, SLBoolean async) {
    (void)self; (void)async;
    return SL_RESULT_SUCCESS;
}

static SLresult engine_obj_GetInterface(void *self, SLInterfaceID iid,
                                        void **out) {
    (void)self;
    if (iid == v_IID_ENGINE) {
        if (out) *out = &g_eng_itf_ptr;
    } else {
        static void *stub_vt[8];
        static void *stub_ptr;
        static int inited = 0;
        if (!inited) {
            for (int i = 0; i < 8; i++) stub_vt[i] = (void *)stub_success;
            stub_ptr = stub_vt;
            inited = 1;
        }
        if (out) *out = &stub_ptr;
    }
    return SL_RESULT_SUCCESS;
}

static void engine_obj_Destroy(void *self) {
    (void)self;
    if (g_audio_dev && p_SDL_CloseAudioDevice) {
        p_SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
        g_audio_initialized = 0;
    }
}

static void init_engine(void) {
    static int inited = 0;
    if (inited) return;
    inited = 1;
    for (int i = 0; i < 8; i++) g_eng_obj_vt[i] = (void *)stub_success;
    g_eng_obj_vt[0] = (void *)engine_obj_Realize;
    g_eng_obj_vt[2] = (void *)obj_GetState;
    g_eng_obj_vt[3] = (void *)engine_obj_GetInterface;
    g_eng_obj_vt[6] = (void *)engine_obj_Destroy;
    g_eng_obj_ptr = g_eng_obj_vt;
    for (int i = 0; i < 16; i++) g_eng_itf_vt[i] = (void *)engine_Unsupported;
    g_eng_itf_vt[2] = (void *)engine_CreateAudioPlayer;
    g_eng_itf_vt[7] = (void *)engine_CreateOutputMix;
    g_eng_itf_ptr = g_eng_itf_vt;
}

static SLresult slCreateEngine_impl(void **pEngine, SLuint32 numOptions,
                                    const void *pEngineOptions,
                                    SLuint32 numInterfaces,
                                    const SLInterfaceID *ids,
                                    const SLBoolean *required) {
    (void)numOptions; (void)pEngineOptions; (void)numInterfaces;
    (void)ids; (void)required;
    fprintf(stderr, "[SL] slCreateEngine (FMOD on the OpenSL path)\n");
    init_engine();
    if (pEngine) *pEngine = &g_eng_obj_ptr;
    return SL_RESULT_SUCCESS;
}

/* -------------------------------------------------------------- pump */
static void pump_callbacks_impl(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        AudioPlayer *p = &g_players[i];
        if (!p->active || p->play_state != SL_PLAYSTATE_PLAYING) continue;

        uint32_t readable = ring_readable(p);
        uint32_t cb_threshold = p->last_enqueue_size;
        if (!cb_threshold || cb_threshold > RING_BYTES / 2)
            cb_threshold = RING_BYTES / 4;
        /* modest slack: ~2x the SDL buffer queued; deeper rings made the
         * pump a CPU grinder on the reference ports without helping. */
        uint32_t refill = cb_threshold * 2;
        uint32_t floor_bytes = SDL_AUDIO_SAMPLES * 2 * 2 * 2;
        if (refill < floor_bytes) refill = floor_bytes;
        if (refill > RING_BYTES / 2) refill = RING_BYTES / 2;

        int max_calls = 4;
        while (p->callback && readable <= refill && max_calls > 0) {
            uint32_t before = p->enqueue_counter;
            { static int n; if (n++ < 8)
                fprintf(stderr, "[SL] pump: bq callback #%d (readable=%u thr=%u)\n",
                        n, readable, refill); }
            p->callback(&p->bq_ptr, p->callback_context);
            if (p->ever_enqueued && !p->decoder_done &&
                p->enqueue_counter == before) {
                p->decoder_done = 1;
                break;
            }
            readable = ring_readable(p);
            max_calls--;
        }

        if (!p->callback && p->ever_enqueued && !p->decoder_done &&
            p->queued_count == 0 && readable == 0)
            p->decoder_done = 1;

        /* HEADATEND once decoder done and ring drained */
        if (p->decoder_done && !p->headatend_fired &&
            ring_readable(p) == 0) {
            p->headatend_fired = 1;
            if (p->play_callback &&
                (p->play_event_mask & SL_PLAYEVENT_HEADATEND))
                p->play_callback(&p->play_ptr, p->play_callback_context,
                                 SL_PLAYEVENT_HEADATEND);
            if (g_audio_dev && p_SDL_LockAudioDevice)
                p_SDL_LockAudioDevice(g_audio_dev);
            p->play_state = SL_PLAYSTATE_STOPPED;
            queue_reset(p);
            if (g_audio_dev && p_SDL_UnlockAudioDevice)
                p_SDL_UnlockAudioDevice(g_audio_dev);
        }
    }
}

/* ------------------------------------------- dlsym entry (bionic.c) */
void *gds_opensles_sym(const char *name) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("GDS_AUDIO");
        enabled = !(e && atoi(e) == 0);   /* GDS_AUDIO=0 -> silence (0.78) */
        if (!enabled)
            fprintf(stderr, "[SL] GDS_AUDIO=0 -- OpenSL shim disabled\n");
    }
    if (!enabled) return NULL;
    /* device-visible proof of which SL entry points libunity reaches for
     * (0.79 logged ZERO of these -> FMOD died before engine creation; after
     * the AudioManager fix in jni.c these must appear). */
    {
        static char seen[256];
        char tag[40];
        snprintf(tag, sizeof tag, "|%s|", name ? name : "?");
        if (!strstr(seen, tag) &&
            strlen(seen) + strlen(tag) < sizeof seen - 1) {
            strcat(seen, tag);
            fprintf(stderr, "[SL] dlsym %s\n", name ? name : "(null)");
        }
    }
    if (!strcmp(name, "slCreateEngine")) return (void *)slCreateEngine_impl;
    if (!strcmp(name, "SL_IID_ENGINE")) return (void *)&v_IID_ENGINE;
    if (!strcmp(name, "SL_IID_PLAY")) return (void *)&v_IID_PLAY;
    if (!strcmp(name, "SL_IID_VOLUME")) return (void *)&v_IID_VOLUME;
    if (!strcmp(name, "SL_IID_BUFFERQUEUE")) return (void *)&v_IID_BUFFERQUEUE;
    if (!strcmp(name, "SL_IID_EFFECTSEND")) return (void *)&v_IID_EFFECTSEND;
    if (!strcmp(name, "SL_IID_ENGINECAPABILITIES"))
        return (void *)&v_IID_ENGINECAPABILITIES;
    if (!strcmp(name, "SL_IID_ENVIRONMENTALREVERB"))
        return (void *)&v_IID_ENVIRONMENTALREVERB;
    if (!strcmp(name, "SL_IID_ANDROIDCONFIGURATION"))
        return (void *)&v_IID_ANDROIDCONFIGURATION;
    if (!strcmp(name, "SL_IID_RECORD")) return (void *)&v_IID_RECORD;
    if (!strcmp(name, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"))
        return (void *)&v_IID_ANDROIDSIMPLEBUFFERQUEUE;
    return NULL;
}

/* Old audio.c contract kept for main.c: audio is lazy (starts when FMOD
 * creates its first player), so start() is a resolved-OK check only. */
/* ================================================================= FMOD
 * AudioTrack output driver ("the missing Java thread").
 * Device evidence (0.84.x runs): FMOD reads the AudioManager property
 * chain (44100/256) but NEVER dlopens libOpenSLES, and no slCreateEngine
 * ever fires -- Unity 2022.3.62's embedded FMOD selects its AudioTrack
 * output (fmod_output_audiotrack.cpp) here.  In a real app the data pump
 * is org.fmod.FMODAudioDevice.run() (Java): loop fmodProcess(ByteBuffer)
 * + AudioTrack.write.  The C++ side registers those natives through JNI
 * RegisterNatives and drives start/stop/close/isRunning on the object.
 * This thread IS the Java run(): wait for start, then pull blocks from
 * the RegisterNatives'd fmodProcess into a synthetic mix player slot, so
 * the existing SDL callback mixes FMOD's PCM like any OpenSL player. */
#include "gds.h"

static AudioPlayer *g_fmod_player;
static int g_fmod_rate, g_fmod_channels, g_fmod_blockframes;

void gds_audio_fmod_config(int rate, int channels, int blockframes)
{
    if (rate > 0 && rate <= 192000) g_fmod_rate = rate;
    if (channels > 0 && channels <= 8) g_fmod_channels = channels;
    if (blockframes > 0 && blockframes <= 16384) g_fmod_blockframes = blockframes;
    fprintf(stderr, "[audio] FMOD startAudioDevice rate=%d ch=%d blockframes=%d\n",
            g_fmod_rate, g_fmod_channels, g_fmod_blockframes);
}

static void *fmod_java_thread(void *arg)
{
    (void)arg;
    typedef int (*fmod_process_fn)(void *env, void *self, void *bb);
    fmod_process_fn proc = NULL;
    int waiting_run = 0, waiting_proc = 0, err_n = 0;
    unsigned long blocks = 0;
    fprintf(stderr, "[audio] FMOD AudioTrack java-thread watching\n");
    for (;;) {
        if (!gds_jni_fmod_should_run()) {
            if (!waiting_run) {
                waiting_run = 1;
                fprintf(stderr, "[audio] fmod-thread: waiting for FMODAudioDevice.start\n");
            }
            usleep(100000);
            continue;
        }
        if (!proc) {
            proc = (fmod_process_fn)gds_jni_native("org/fmod/FMODAudioDevice",
                                                   "fmodProcess");
            if (!proc) {
                if (!waiting_proc) {
                    waiting_proc = 1;
                    fprintf(stderr, "[audio] fmod-thread: start seen, "
                                    "fmodProcess native not registered yet\n");
                }
                usleep(50000);
                continue;
            }
            fprintf(stderr, "[audio] fmodProcess native at %p\n", (void *)proc);
            /* fmodProcess(GetDirectBufferCapacity) decides the block size.
             * Default direct buffer is 65536B (~372ms) -- far too coarse:
             * shrink to 8192B = 2048 stereo frames at 44.1kHz (~46ms), the
             * same cadence as the SDL device.  FMOD re-queries capacity on
             * every call (libunity.so @0xbe8d08), so this is safe. */
            gds_jni_fmod_set_buffer_size(8192);
        }
        if (!g_fmod_player) {
            g_fmod_player = alloc_player();
            if (g_fmod_player) {
                g_fmod_player->sample_rate     = (SLuint32)(g_fmod_rate > 0 ? g_fmod_rate : 44100);
                g_fmod_player->num_channels    = (SLuint32)(g_fmod_channels > 0 ? g_fmod_channels : 2);
                g_fmod_player->bits_per_sample = 16;
                g_fmod_player->play_state      = SL_PLAYSTATE_PLAYING;
                g_fmod_player->volume          = 0.8f;
            }
            ensure_audio_initialized();
            fprintf(stderr, "[audio] fmod-thread: pumping (rate=%d ch=%d blockframes=%d bufsize=%d)\n",
                    g_fmod_rate > 0 ? g_fmod_rate : 44100,
                    g_fmod_channels > 0 ? g_fmod_channels : 2,
                    g_fmod_blockframes, gds_jni_fmod_buffer_size());
        }
        /* 0.85 evidence: fmodProcess returns 0 on SUCCESS (fills the whole
         * direct buffer, zero-padding on underrun like the real Java run()
         * loop which writes `capacity` bytes regardless) and -1 only while
         * the C++ output object ([0x102b2b0]) doesn't exist yet.  0.85.0
         * treated 0 as "no data" and never wrote a byte. */
        unsigned rate = g_fmod_rate > 0 ? (unsigned)g_fmod_rate : 44100;
        unsigned ch = g_fmod_channels > 0 ? (unsigned)g_fmod_channels : 2;
        unsigned bufsz = (unsigned)gds_jni_fmod_buffer_size();
        if (!bufsz) bufsz = 8192;
        /* pace like AudioTrack.write(): it blocks while the track is full.
         * Keep ~5 blocks queued (~232ms worst-case click latency) instead
         * of the old 'grow until 4MB ring is nearly full' throttle, which
         * would have stacked up ~24s of audio lag. */
        while (g_fmod_player &&
               ring_readable(g_fmod_player) >= bufsz * 5) {
            if (!gds_jni_fmod_should_run()) break;
            usleep(4000);
        }
        int rc = proc(gds_jni_env(), gds_jni_fmod_device(),
                      gds_jni_fmod_bytebuffer());
        if (rc >= 0) {
            if (g_fmod_player)
                ring_write(g_fmod_player, gds_jni_fmod_pcm(), bufsz);
            blocks++;
            if (blocks <= 3 || blocks % 600 == 0) {
                fprintf(stderr, "[audio] fmod block #%lu (rc=%d %u bytes, rate=%u, fill=%u)\n",
                        blocks, rc, bufsz, rate,
                        g_fmod_player ? ring_readable(g_fmod_player) : 0);
                fflush(stderr);
            }
            err_n = 0;
        } else {
            if (err_n < 6) {
                err_n++;
                fprintf(stderr, "[audio] fmodProcess -> %d (#%lu; -1 = FMOD output not up yet)\n",
                        rc, blocks);
                fflush(stderr);
            }
            usleep(4000);
        }
    }
    return NULL;
}

void gds_audio_fmod_thread_start(void)
{
    static int started;
    if (started) return;
    started = 1;
    pthread_t pt;
    if (pthread_create(&pt, NULL, fmod_java_thread, NULL) == 0)
        pthread_detach(pt);
}

int gds_audio_start(void *env) {
    (void)env;
    /* 0.85: the FMOD AudioTrack java-thread runs on its own so FMOD's
     * start() (which lands AFTER nativeResume, inside nativeRender) is
     * picked up whenever it fires.  One thread, permanent; it idles at
     * 100ms cadence until FMODAudioDevice.start actually happens. */
    gds_audio_fmod_thread_start();
    return 1;
}

void gds_audio_stop(void) {
    if (g_audio_dev && p_SDL_CloseAudioDevice)
        p_SDL_CloseAudioDevice(g_audio_dev);
    g_audio_dev = 0;
    g_audio_initialized = 0;
}
