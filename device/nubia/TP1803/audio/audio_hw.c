/*
 * Minimal primary audio HAL for Nubia TP1803 (SM8150 + WCD9340 "tavil") on
 * Android 6 / CM13.
 *
 * Qualcomm's real HAL and its ACDB calibration are Android 10+ binaries, so
 * this drives the ALSA card directly with tinyalsa and routes with
 * audio_route using the stock mixer_paths_tavil.xml:
 *   playback: MultiMedia1 (PCM 0), "deep-buffer-playback" + device path
 *   capture:  MultiMedia1 (PCM 0), "audio-record" + mic path
 */
#define LOG_TAG "audio_hw_tp1803"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <cutils/log.h>
#include <cutils/properties.h>
#include <cutils/str_parms.h>
#include <hardware/audio.h>
#include <hardware/hardware.h>
#include <system/audio.h>
#include <tinyalsa/asoundlib.h>
#include <audio_route/audio_route.h>

#define CARD             0
#define PCM_DEVICE       0
#define MIXER_XML        "/system/etc/mixer_paths_tavil.xml"
#define OUT_RATE         48000
#define OUT_PERIOD_SIZE  960     /* 20 ms */
#define OUT_PERIOD_COUNT 4
#define IN_RATE          48000
#define IN_PERIOD_SIZE   960
#define IN_PERIOD_COUNT  4

struct tp_audio_device {
    struct audio_hw_device hw;
    pthread_mutex_t lock;
    struct audio_route *ar;
    audio_devices_t out_device;
    audio_devices_t in_device;
    bool mic_mute;
    struct tp_stream_out *out;
    struct tp_stream_in *in;
    /* voice call: hostless VoiceMMode1 front end, audio runs in the DSP */
    int in_call;
    struct pcm *voice_rx, *voice_tx;
    float voice_volume;
};

struct tp_stream_out {
    struct audio_stream_out stream;
    pthread_mutex_t lock;
    struct tp_audio_device *dev;
    struct pcm *pcm;
    bool standby;
    audio_devices_t device;
    uint64_t written;
};

struct tp_stream_in {
    struct audio_stream_in stream;
    pthread_mutex_t lock;
    struct tp_audio_device *dev;
    struct pcm *pcm;
    bool standby;
    audio_devices_t device;
    uint32_t channels;
    unsigned read_errors;   /* logged once per stream */
    unsigned muted_reads;
};

static const struct pcm_config out_config = {
    .channels = 2, .rate = OUT_RATE, .period_size = OUT_PERIOD_SIZE,
    .period_count = OUT_PERIOD_COUNT, .format = PCM_FORMAT_S16_LE,
};

/* ---------------------------------------------------------------- routing */

static const char *out_device_path(audio_devices_t d)
{
    if (d & (AUDIO_DEVICE_OUT_WIRED_HEADSET | AUDIO_DEVICE_OUT_WIRED_HEADPHONE))
        return "headphones";
    if (d & AUDIO_DEVICE_OUT_EARPIECE)
        return "handset";
    return "speaker";
}

static const char *in_device_path(audio_devices_t d)
{
    /* Input device values all carry AUDIO_DEVICE_BIT_IN: mask it off, or
     * every device "matches" the wired headset. */
    if (d & AUDIO_DEVICE_IN_WIRED_HEADSET & ~AUDIO_DEVICE_BIT_IN)
        return "headset-mic";
    return "handset-mic";
}

/* Apply "<usecase> <device>" if the XML has it, else "<usecase>", plus the
 * device path. Caller holds dev->lock. */
static void apply_route(struct tp_audio_device *dev, const char *usecase, const char *device)
{
    char name[96];
    snprintf(name, sizeof(name), "%s %s", usecase, device);
    if (audio_route_apply_path(dev->ar, name) < 0)
        audio_route_apply_path(dev->ar, usecase);
    audio_route_apply_path(dev->ar, device);
}

/* Microphone that goes with an output device during a call. */
static const char *voice_mic_path(audio_devices_t out)
{
    if (out & AUDIO_DEVICE_OUT_WIRED_HEADSET)
        return "headset-mic";
    if (out & AUDIO_DEVICE_OUT_SPEAKER)
        return "speaker-mic";
    return "handset-mic";
}

static void update_routes(struct tp_audio_device *dev)
{
    audio_route_reset(dev->ar);
    if (dev->in_call) {
        audio_devices_t o = dev->out_device ? dev->out_device : AUDIO_DEVICE_OUT_EARPIECE;
        apply_route(dev, "voicemmode1-call", out_device_path(o));
        audio_route_apply_path(dev->ar, voice_mic_path(o));
        if (!strcmp(out_device_path(o), "handset"))
            audio_route_apply_path(dev->ar, "tp1803-voice-handset-gain");
    }
    if (dev->out && !dev->out->standby)
        apply_route(dev, "deep-buffer-playback", out_device_path(dev->out->device));
    if (dev->in && !dev->in->standby)
        apply_route(dev, "audio-record", in_device_path(dev->in->device));
    audio_route_update_mixer(dev->ar);
}

/* ---------------------------------------------------------------- output */

struct tp_audio_device;
static void voice_start(struct tp_audio_device *dev);
static void voice_stop(struct tp_audio_device *dev);

static uint32_t out_get_sample_rate(const struct audio_stream *s) { (void)s; return OUT_RATE; }
static int out_set_sample_rate(struct audio_stream *s, uint32_t r) { (void)s; return r == OUT_RATE ? 0 : -ENOSYS; }
static size_t out_get_buffer_size(const struct audio_stream *s) { (void)s; return OUT_PERIOD_SIZE * 2 * sizeof(int16_t); }
static audio_channel_mask_t out_get_channels(const struct audio_stream *s) { (void)s; return AUDIO_CHANNEL_OUT_STEREO; }
static audio_format_t out_get_format(const struct audio_stream *s) { (void)s; return AUDIO_FORMAT_PCM_16_BIT; }
static int out_set_format(struct audio_stream *s, audio_format_t f) { (void)s; (void)f; return -ENOSYS; }
static int out_dump(const struct audio_stream *s, int fd) { (void)s; (void)fd; return 0; }
static int out_add_audio_effect(const struct audio_stream *s, effect_handle_t e) { (void)s; (void)e; return 0; }
static int out_remove_audio_effect(const struct audio_stream *s, effect_handle_t e) { (void)s; (void)e; return 0; }
static uint32_t out_get_latency(const struct audio_stream_out *s)
{
    (void)s;
    return OUT_PERIOD_SIZE * OUT_PERIOD_COUNT * 1000 / OUT_RATE;
}
static int out_set_volume(struct audio_stream_out *s, float l, float r) { (void)s; (void)l; (void)r; return -ENOSYS; }

static int do_out_standby(struct tp_stream_out *out)
{
    if (!out->standby) {
        pcm_close(out->pcm);
        out->pcm = NULL;
        out->standby = true;
        pthread_mutex_lock(&out->dev->lock);
        update_routes(out->dev);
        pthread_mutex_unlock(&out->dev->lock);
    }
    return 0;
}

static int out_standby(struct audio_stream *stream)
{
    struct tp_stream_out *out = (struct tp_stream_out *)stream;
    pthread_mutex_lock(&out->lock);
    do_out_standby(out);
    pthread_mutex_unlock(&out->lock);
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct tp_stream_out *out = (struct tp_stream_out *)stream;
    struct str_parms *parms = str_parms_create_str(kvpairs);
    char value[32];
    if (str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value)) >= 0) {
        audio_devices_t d = atoi(value);
        if (d) {
            pthread_mutex_lock(&out->lock);
            /* The codec's SLIMbus port set is fixed while the SLIM_0_RX
             * backend runs: switching speaker (RX0+RX1) <-> handset (RX0)
             * under a live stream stalls it ("overflow error on RX port 1",
             * silent earpiece in calls). Like the stock HAL, stop every
             * user of the backend first; out_write reopens it. */
            if (d != out->device && !out->standby)
                do_out_standby(out);
            out->device = d;
            pthread_mutex_lock(&out->dev->lock);
            audio_devices_t old = out->dev->out_device;
            out->dev->out_device = d;
            if (out->dev->in_call && d != old) {
                /* new device pair needs its own voice calibration */
                voice_stop(out->dev);
                voice_start(out->dev);
            } else if (!out->standby || out->dev->in_call) {
                update_routes(out->dev);
            }
            pthread_mutex_unlock(&out->dev->lock);
            pthread_mutex_unlock(&out->lock);
        }
    }
    str_parms_destroy(parms);
    return 0;
}

static char *out_get_parameters(const struct audio_stream *s, const char *keys)
{
    (void)s; (void)keys;
    return strdup("");
}

static ssize_t out_write(struct audio_stream_out *stream, const void *buffer, size_t bytes)
{
    struct tp_stream_out *out = (struct tp_stream_out *)stream;
    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        struct pcm_config cfg = out_config;
        out->pcm = pcm_open(CARD, PCM_DEVICE, PCM_OUT | PCM_MONOTONIC, &cfg);
        if (!pcm_is_ready(out->pcm)) {
            ALOGE("pcm_open(out) failed: %s", pcm_get_error(out->pcm));
            pcm_close(out->pcm);
            out->pcm = NULL;
            pthread_mutex_unlock(&out->lock);
            usleep(bytes * 1000000 / 4 / OUT_RATE);
            return bytes;
        }
        out->standby = false;
        pthread_mutex_lock(&out->dev->lock);
        update_routes(out->dev);
        pthread_mutex_unlock(&out->dev->lock);
    }
    if (pcm_write(out->pcm, buffer, bytes) != 0)
        ALOGW("pcm_write: %s", pcm_get_error(out->pcm));
    out->written += bytes / 4;
    pthread_mutex_unlock(&out->lock);
    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *s, uint32_t *dsp_frames)
{
    (void)s; (void)dsp_frames;
    return -EINVAL;
}

static int out_get_presentation_position(const struct audio_stream_out *stream,
                                         uint64_t *frames, struct timespec *timestamp)
{
    struct tp_stream_out *out = (struct tp_stream_out *)stream;
    int ret = -ENODATA;
    pthread_mutex_lock(&out->lock);
    if (out->pcm) {
        unsigned int avail;
        if (pcm_get_htimestamp(out->pcm, &avail, timestamp) == 0) {
            int64_t kernel_buffer = (int64_t)OUT_PERIOD_SIZE * OUT_PERIOD_COUNT;
            int64_t signed_frames = (int64_t)out->written - kernel_buffer + avail;
            if (signed_frames >= 0) {
                *frames = signed_frames;
                ret = 0;
            }
        }
    }
    pthread_mutex_unlock(&out->lock);
    return ret;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *s, int64_t *t)
{
    (void)s; (void)t;
    return -EINVAL;
}

/* ---------------------------------------------------------------- input */

static uint32_t in_get_sample_rate(const struct audio_stream *s) { (void)s; return IN_RATE; }
static int in_set_sample_rate(struct audio_stream *s, uint32_t r) { (void)s; (void)r; return -ENOSYS; }
static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct tp_stream_in *in = (struct tp_stream_in *)stream;
    return IN_PERIOD_SIZE * in->channels * sizeof(int16_t);
}
static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
    struct tp_stream_in *in = (struct tp_stream_in *)stream;
    return in->channels == 2 ? AUDIO_CHANNEL_IN_STEREO : AUDIO_CHANNEL_IN_MONO;
}
static audio_format_t in_get_format(const struct audio_stream *s) { (void)s; return AUDIO_FORMAT_PCM_16_BIT; }
static int in_set_format(struct audio_stream *s, audio_format_t f) { (void)s; (void)f; return -ENOSYS; }
static int in_dump(const struct audio_stream *s, int fd) { (void)s; (void)fd; return 0; }
static int in_set_gain(struct audio_stream_in *s, float g) { (void)s; (void)g; return 0; }
static uint32_t in_get_input_frames_lost(struct audio_stream_in *s) { (void)s; return 0; }
static int in_add_audio_effect(const struct audio_stream *s, effect_handle_t e) { (void)s; (void)e; return 0; }
static int in_remove_audio_effect(const struct audio_stream *s, effect_handle_t e) { (void)s; (void)e; return 0; }
static char *in_get_parameters(const struct audio_stream *s, const char *keys) { (void)s; (void)keys; return strdup(""); }

static int do_in_standby(struct tp_stream_in *in)
{
    if (!in->standby) {
        pcm_close(in->pcm);
        in->pcm = NULL;
        in->standby = true;
        pthread_mutex_lock(&in->dev->lock);
        update_routes(in->dev);
        pthread_mutex_unlock(&in->dev->lock);
    }
    return 0;
}

static int in_standby(struct audio_stream *stream)
{
    struct tp_stream_in *in = (struct tp_stream_in *)stream;
    pthread_mutex_lock(&in->lock);
    do_in_standby(in);
    pthread_mutex_unlock(&in->lock);
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct tp_stream_in *in = (struct tp_stream_in *)stream;
    struct str_parms *parms = str_parms_create_str(kvpairs);
    char value[32];
    if (str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value)) >= 0) {
        audio_devices_t d = atoi(value);
        if (d) {
            pthread_mutex_lock(&in->lock);
            in->device = d;
            pthread_mutex_lock(&in->dev->lock);
            if (!in->standby)
                update_routes(in->dev);
            pthread_mutex_unlock(&in->dev->lock);
            pthread_mutex_unlock(&in->lock);
        }
    }
    str_parms_destroy(parms);
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void *buffer, size_t bytes)
{
    struct tp_stream_in *in = (struct tp_stream_in *)stream;
    pthread_mutex_lock(&in->lock);
    if (in->standby) {
        struct pcm_config cfg = {
            .channels = in->channels, .rate = IN_RATE, .period_size = IN_PERIOD_SIZE,
            .period_count = IN_PERIOD_COUNT, .format = PCM_FORMAT_S16_LE,
        };
        pthread_mutex_lock(&in->dev->lock);
        in->standby = false;
        /* AudioFlinger can hold several input streams open; route for the
         * one that is actually starting to capture. */
        in->dev->in = in;
        update_routes(in->dev);       /* route before starting capture */
        ALOGI("capture start: %u ch, device 0x%x", in->channels, in->device);
        pthread_mutex_unlock(&in->dev->lock);
        in->pcm = pcm_open(CARD, PCM_DEVICE, PCM_IN, &cfg);
        if (!pcm_is_ready(in->pcm)) {
            ALOGE("pcm_open(in) failed: %s", pcm_get_error(in->pcm));
            pcm_close(in->pcm);
            in->pcm = NULL;
            in->standby = true;
            pthread_mutex_unlock(&in->lock);
            memset(buffer, 0, bytes);
            usleep(bytes * 1000000 / (in->channels * 2) / IN_RATE);
            return bytes;
        }
    }
    if (pcm_read(in->pcm, buffer, bytes) != 0) {
        ALOGE_IF(!in->read_errors++, "pcm_read(in, %zu) failed: %s", bytes, pcm_get_error(in->pcm));
        memset(buffer, 0, bytes);
    }
    if (in->dev->mic_mute) {
        ALOGW_IF(!in->muted_reads++, "mic muted: capturing silence");
        memset(buffer, 0, bytes);
    }
    pthread_mutex_unlock(&in->lock);
    return bytes;
}

/* ---------------------------------------------------------------- device */

static int adev_open_output_stream(struct audio_hw_device *hw, audio_io_handle_t handle,
                                   audio_devices_t devices, audio_output_flags_t flags,
                                   struct audio_config *config, struct audio_stream_out **stream_out,
                                   const char *address)
{
    struct tp_audio_device *dev = (struct tp_audio_device *)hw;
    (void)handle; (void)flags; (void)address;

    if (config->sample_rate != OUT_RATE || config->format != AUDIO_FORMAT_PCM_16_BIT ||
        config->channel_mask != AUDIO_CHANNEL_OUT_STEREO) {
        config->sample_rate = OUT_RATE;
        config->format = AUDIO_FORMAT_PCM_16_BIT;
        config->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
        return -EINVAL;
    }
    pthread_mutex_lock(&dev->lock);
    if (dev->out) {
        pthread_mutex_unlock(&dev->lock);
        return -ENOSYS;   /* single primary output */
    }
    pthread_mutex_unlock(&dev->lock);

    struct tp_stream_out *out = calloc(1, sizeof(*out));
    if (!out)
        return -ENOMEM;
    pthread_mutex_init(&out->lock, NULL);
    out->dev = dev;
    out->standby = true;
    out->device = devices ? devices : AUDIO_DEVICE_OUT_SPEAKER;
    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
    out->stream.get_presentation_position = out_get_presentation_position;

    pthread_mutex_lock(&dev->lock);
    dev->out = out;
    pthread_mutex_unlock(&dev->lock);
    *stream_out = &out->stream;
    return 0;
}

static void adev_close_output_stream(struct audio_hw_device *hw, struct audio_stream_out *stream)
{
    struct tp_audio_device *dev = (struct tp_audio_device *)hw;
    struct tp_stream_out *out = (struct tp_stream_out *)stream;
    out_standby(&stream->common);
    pthread_mutex_lock(&dev->lock);
    if (dev->out == out)
        dev->out = NULL;
    pthread_mutex_unlock(&dev->lock);
    free(out);
}

static int adev_open_input_stream(struct audio_hw_device *hw, audio_io_handle_t handle,
                                  audio_devices_t devices, struct audio_config *config,
                                  struct audio_stream_in **stream_in, audio_input_flags_t flags,
                                  const char *address, audio_source_t source)
{
    struct tp_audio_device *dev = (struct tp_audio_device *)hw;
    (void)handle; (void)flags; (void)address; (void)source;

    if (config->sample_rate != IN_RATE || config->format != AUDIO_FORMAT_PCM_16_BIT ||
        (config->channel_mask != AUDIO_CHANNEL_IN_MONO && config->channel_mask != AUDIO_CHANNEL_IN_STEREO)) {
        config->sample_rate = IN_RATE;
        config->format = AUDIO_FORMAT_PCM_16_BIT;
        config->channel_mask = AUDIO_CHANNEL_IN_MONO;
        return -EINVAL;
    }
    struct tp_stream_in *in = calloc(1, sizeof(*in));
    if (!in)
        return -ENOMEM;
    pthread_mutex_init(&in->lock, NULL);
    in->dev = dev;
    in->standby = true;
    in->device = devices;
    in->channels = config->channel_mask == AUDIO_CHANNEL_IN_STEREO ? 2 : 1;
    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    pthread_mutex_lock(&dev->lock);
    dev->in = in;
    pthread_mutex_unlock(&dev->lock);
    *stream_in = &in->stream;
    return 0;
}

static void adev_close_input_stream(struct audio_hw_device *hw, struct audio_stream_in *stream)
{
    struct tp_audio_device *dev = (struct tp_audio_device *)hw;
    struct tp_stream_in *in = (struct tp_stream_in *)stream;
    in_standby(&stream->common);
    pthread_mutex_lock(&dev->lock);
    if (dev->in == in)
        dev->in = NULL;
    pthread_mutex_unlock(&dev->lock);
    free(in);
}

static int adev_set_parameters(struct audio_hw_device *hw, const char *kvpairs) { (void)hw; (void)kvpairs; return 0; }
static char *adev_get_parameters(const struct audio_hw_device *hw, const char *keys) { (void)hw; (void)keys; return strdup(""); }
static int adev_init_check(const struct audio_hw_device *hw) { (void)hw; return 0; }
static void voice_apply_gain(struct tp_audio_device *dev);
static int adev_set_voice_volume(struct audio_hw_device *hw, float v)
{
    struct tp_audio_device *dev = (struct tp_audio_device *)hw;
    pthread_mutex_lock(&dev->lock);
    dev->voice_volume = v;
    if (dev->in_call)
        voice_apply_gain(dev);
    pthread_mutex_unlock(&dev->lock);
    return 0;
}
static int adev_set_master_volume(struct audio_hw_device *hw, float v) { (void)hw; (void)v; return -ENOSYS; }
static int adev_get_master_volume(struct audio_hw_device *hw, float *v) { (void)hw; (void)v; return -ENOSYS; }
static int adev_set_master_mute(struct audio_hw_device *hw, bool m) { (void)hw; (void)m; return -ENOSYS; }
static int adev_get_master_mute(struct audio_hw_device *hw, bool *m) { (void)hw; (void)m; return -ENOSYS; }
/* ---------------------------------------------------------------- ACDB */

/*
 * Qualcomm's calibration loader (stock libacdbloader + /vendor/etc/acdbdata)
 * pushes DSP tuning through /dev/msm_audio_cal. Voice calls need it: without
 * voice calibration the DSP's voice processing passes no audio.
 */
struct acdb_listnode { struct acdb_listnode *next, *prev; };
struct acdb_init_data_v4 {
    char *cvd_version;
    char *snd_card_name;
    struct acdb_listnode *meta_key_list;
    bool *is_instance_id_supported;
};
#define ACDB_LOADER_INIT_V4 4

static void (*acdb_send_voice_cal)(int rx_acdb_id, int tx_acdb_id);

static void acdb_init(void)
{
    void *h = dlopen("libacdbloader.so", RTLD_NOW);
    if (!h) {
        ALOGW("no ACDB loader: %s", dlerror());
        return;
    }
    int (*init_v4)(void *, int) = (int (*)(void *, int))dlsym(h, "acdb_loader_init_v4");
    acdb_send_voice_cal = (void (*)(int, int))dlsym(h, "acdb_loader_send_voice_cal");
    if (!init_v4)
        return;
    static char cvd[100], card[64];
    static struct acdb_listnode keys = { &keys, &keys };
    static bool instance_id;
    struct mixer *m = mixer_open(CARD);
    if (m) {
        struct mixer_ctl *ctl = mixer_get_ctl_by_name(m, "CVD Version");
        if (ctl) {
            int n = mixer_ctl_get_num_values(ctl);
            if (n > (int)sizeof(cvd) - 1)
                n = sizeof(cvd) - 1;
            mixer_ctl_get_array(ctl, cvd, n);
        }
        snprintf(card, sizeof(card), "%s", mixer_get_name(m));
        mixer_close(m);
    }
    struct acdb_init_data_v4 d = { cvd, card, &keys, &instance_id };
    int r = init_v4(&d, ACDB_LOADER_INIT_V4);
    ALOGI("ACDB init (card %s, CVD %s): %d", card, cvd, r);
    if (r) {
        acdb_send_voice_cal = NULL;
        return;
    }
    /* Register the custom DSP topologies the calibration refers to; without
     * this the voice processor's topology commit fails. */
    int (*send_common_top)(void) = (int (*)(void))dlsym(h, "acdb_loader_send_common_custom_topology");
    if (send_common_top)
        ALOGI("ACDB common custom topology: %d", send_common_top());
}

/* ---------------------------------------------------------------- voice */

#define VOICE_PCM_DEVICE   2            /* VoiceMMode1 */
#define ALL_SESSION_VSID   0xFFFFFFFFu
#define VOICE_RAMP_MS      20

static void voice_mixer_array(const char *name, const long *vals, int n)
{
    struct mixer *m = mixer_open(CARD);
    if (!m)
        return;
    struct mixer_ctl *ctl = mixer_get_ctl_by_name(m, name);
    if (ctl) {
        /* One write with all values: per-element mixer_ctl_set_value does a
         * read-modify-write, and these controls read back as -1, so every
         * partial write was rejected by the kernel (EINVAL). */
        if (n > (int)mixer_ctl_get_num_values(ctl))
            n = mixer_ctl_get_num_values(ctl);
        int r = mixer_ctl_set_array(ctl, vals, n);
        if (r)
            ALOGW("%s <- %ld: %d", name, vals[0], r);
    } else {
        ALOGW("no mixer control '%s'", name);
    }
    mixer_close(m);
}

static void voice_apply_gain(struct tp_audio_device *dev)
{
    /* "Voice Rx Gain" takes an inverted volume-table index (stock
     * platform_set_voice_volume): 0 = loudest .. steps = quietest, with
     * ro.config.vc_call_vol_steps (11 on TP1803) steps. */
    float v = dev->voice_volume < 0 ? 0 : dev->voice_volume > 1 ? 1 : dev->voice_volume;
    int steps = property_get_int32("ro.config.vc_call_vol_steps", 11);
    long idx = (long)((1.0f - v) * steps + 0.5f);
    long g[3] = { idx, (long)ALL_SESSION_VSID, VOICE_RAMP_MS };
    ALOGI("voice volume %.2f -> rx gain index %ld/%d", v, idx, steps);
    voice_mixer_array("Voice Rx Gain", g, 3);
    long m[3] = { dev->mic_mute ? 1 : 0, (long)ALL_SESSION_VSID, VOICE_RAMP_MS };
    voice_mixer_array("Voice Tx Mute", m, 3);
}

/* Caller holds dev->lock. */
static void voice_start(struct tp_audio_device *dev)
{
    /* M's tinyalsa writes defaults back into the config: keep it writable
     * (and separate per stream). */
    struct pcm_config rx_cfg = {
        .channels = 1, .rate = 8000, .period_size = 160, .period_count = 2,
        .format = PCM_FORMAT_S16_LE,
    };
    struct pcm_config tx_cfg = rx_cfg;
    dev->in_call = 1;
    update_routes(dev);
    if (acdb_send_voice_cal) {
        /* ACDB device ids (stock audio_platform_info / platform.c) */
        audio_devices_t o = dev->out_device ? dev->out_device : AUDIO_DEVICE_OUT_EARPIECE;
        int rx = 7, tx = 4;                               /* handset */
        if (o & AUDIO_DEVICE_OUT_SPEAKER) {
            rx = 14; tx = 11;                             /* voice speaker */
        } else if (o & (AUDIO_DEVICE_OUT_WIRED_HEADSET | AUDIO_DEVICE_OUT_WIRED_HEADPHONE)) {
            rx = 10; tx = (o & AUDIO_DEVICE_OUT_WIRED_HEADSET) ? 8 : 4;
        }
        /* debug: persist.tp1803.voice_acdb=<rx>,<tx> overrides the ids */
        char ov[PROPERTY_VALUE_MAX];
        if (property_get("persist.tp1803.voice_acdb", ov, "") > 0)
            sscanf(ov, "%d,%d", &rx, &tx);
        ALOGI("voice cal rx %d tx %d", rx, tx);
        acdb_send_voice_cal(rx, tx);
    }
    dev->voice_rx = pcm_open(CARD, VOICE_PCM_DEVICE, PCM_OUT, &rx_cfg);
    dev->voice_tx = pcm_open(CARD, VOICE_PCM_DEVICE, PCM_IN, &tx_cfg);
    if (!pcm_is_ready(dev->voice_rx) || !pcm_is_ready(dev->voice_tx)) {
        ALOGE("voice pcm open failed: rx %s / tx %s", pcm_get_error(dev->voice_rx),
              pcm_get_error(dev->voice_tx));
    } else {
        if (pcm_start(dev->voice_rx) < 0)
            ALOGE("voice rx start: %s", pcm_get_error(dev->voice_rx));
        if (pcm_start(dev->voice_tx) < 0)
            ALOGE("voice tx start: %s", pcm_get_error(dev->voice_tx));
    }
    voice_apply_gain(dev);
    ALOGI("voice call audio started (out 0x%x)", dev->out_device);
}

static void voice_stop(struct tp_audio_device *dev)
{
    if (dev->voice_tx)
        pcm_close(dev->voice_tx);
    if (dev->voice_rx)
        pcm_close(dev->voice_rx);
    dev->voice_rx = dev->voice_tx = NULL;
    dev->in_call = 0;
    update_routes(dev);
    ALOGI("voice call audio stopped");
}

static int adev_set_mode(struct audio_hw_device *hw, audio_mode_t mode)
{
    struct tp_audio_device *dev = (struct tp_audio_device *)hw;
    pthread_mutex_lock(&dev->lock);
    if (mode == AUDIO_MODE_IN_CALL && !dev->in_call)
        voice_start(dev);
    else if (mode != AUDIO_MODE_IN_CALL && dev->in_call)
        voice_stop(dev);
    pthread_mutex_unlock(&dev->lock);
    return 0;
}
static int adev_set_mic_mute(struct audio_hw_device *hw, bool state)
{
    struct tp_audio_device *dev = (struct tp_audio_device *)hw;
    pthread_mutex_lock(&dev->lock);
    dev->mic_mute = state;
    if (dev->in_call)
        voice_apply_gain(dev);
    pthread_mutex_unlock(&dev->lock);
    return 0;
}
static int adev_get_mic_mute(const struct audio_hw_device *hw, bool *state)
{
    *state = ((const struct tp_audio_device *)hw)->mic_mute;
    return 0;
}
static size_t adev_get_input_buffer_size(const struct audio_hw_device *hw, const struct audio_config *config)
{
    (void)hw;
    return IN_PERIOD_SIZE * audio_channel_count_from_in_mask(config->channel_mask) * sizeof(int16_t);
}
static int adev_dump(const audio_hw_device_t *device, int fd) { (void)device; (void)fd; return 0; }

static int adev_close(hw_device_t *device)
{
    struct tp_audio_device *dev = (struct tp_audio_device *)device;
    audio_route_free(dev->ar);
    free(dev);
    return 0;
}

static int adev_open(const hw_module_t *module, const char *name, hw_device_t **device)
{
    if (strcmp(name, AUDIO_HARDWARE_INTERFACE))
        return -EINVAL;
    struct tp_audio_device *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return -ENOMEM;
    dev->voice_volume = 1.0f;
    /* The sound card only registers once the ADSP audio PD is up, which can
     * be after mediaserver starts: wait for it rather than come up mute. */
    int tries;
    for (tries = 0; tries < 300; tries++) {
        struct mixer *probe = mixer_open(CARD);
        if (probe) {
            mixer_close(probe);
            if (tries)
                ALOGI("sound card %d appeared after %d ms", CARD, tries * 100);
            break;
        }
        if (tries == 0)
            ALOGW("sound card %d not ready yet, waiting", CARD);
        usleep(100000);
    }
    dev->ar = audio_route_init(CARD, MIXER_XML);
    acdb_init();
    if (!dev->ar) {
        ALOGE("audio_route_init(%s) failed", MIXER_XML);
        free(dev);
        return -EINVAL;
    }
    pthread_mutex_init(&dev->lock, NULL);
    dev->hw.common.tag = HARDWARE_DEVICE_TAG;
    dev->hw.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    dev->hw.common.module = (struct hw_module_t *)module;
    dev->hw.common.close = adev_close;
    dev->hw.init_check = adev_init_check;
    dev->hw.set_voice_volume = adev_set_voice_volume;
    dev->hw.set_master_volume = adev_set_master_volume;
    dev->hw.get_master_volume = adev_get_master_volume;
    dev->hw.set_master_mute = adev_set_master_mute;
    dev->hw.get_master_mute = adev_get_master_mute;
    dev->hw.set_mode = adev_set_mode;
    dev->hw.set_mic_mute = adev_set_mic_mute;
    dev->hw.get_mic_mute = adev_get_mic_mute;
    dev->hw.set_parameters = adev_set_parameters;
    dev->hw.get_parameters = adev_get_parameters;
    dev->hw.get_input_buffer_size = adev_get_input_buffer_size;
    dev->hw.open_output_stream = adev_open_output_stream;
    dev->hw.close_output_stream = adev_close_output_stream;
    dev->hw.open_input_stream = adev_open_input_stream;
    dev->hw.close_input_stream = adev_close_input_stream;
    dev->hw.dump = adev_dump;
    dev->out_device = AUDIO_DEVICE_OUT_SPEAKER;
    *device = &dev->hw.common;
    ALOGI("TP1803 audio HAL ready (card %d, %s)", CARD, MIXER_XML);
    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "TP1803 tinyalsa audio HAL",
        .author = "TP1803 legacy port",
        .methods = &hal_module_methods,
    },
};
