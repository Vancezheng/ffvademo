/*
 * ffvadecoder.c - FFmpeg/vaapi decoder
 *
 * Copyright (C) 2014 Intel Corporation
 *   Author: Gwenole Beauchesne <gwenole.beauchesne@intel.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301
 */

#include "sysdeps.h"
#include <pthread.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libavcodec/vaapi.h>
#include <libswresample/swresample.h>
#include <SDL.h>
#include <SDL_thread.h>
#include <pthread.h>
#include "ffvadecoder.h"
#include "ffvadisplay.h"
#include "ffvadisplay_priv.h"
#include "ffvasurface.h"
#include "ffmpeg_compat.h"
#include "ffmpeg_utils.h"
#include "vaapi_utils.h"

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
#define MAX_AUDIO_FRAME_SIZE 192000

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0
#define SAMPLE_CORRECTION_PERCENT_MAX 10
#define AUDIO_DIFF_AVG_NB 20

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

#define DEFAULT_AV_SYNC_TYPE AV_SYNC_VIDEO_MASTER

#define VIDEO_PICTURE_QUEUE_SIZE 1

#define FF_REFRESH_EVENT (SDL_USEREVENT)

extern int render_frame(FFVADecoderFrame *dec_frame);
void schedule_refresh(FFVADecoder *dec, int delay);

enum {
    STATE_INITIALIZED   = 1 << 0,
    STATE_OPENED        = 1 << 1,
    STATE_STARTED       = 1 << 2,
};

enum {
    AV_SYNC_AUDIO_MASTER,
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_MASTER,
};

typedef struct PacketQueue {
  AVPacketList *first_pkt, *last_pkt;
  int nb_packets;
  int size;
  SDL_mutex *mutex;
  SDL_cond *cond;
} PacketQueue;

typedef struct AudioParams {
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

struct ffva_decoder_s {
    const void *klass;
    AVFormatContext *fmtctx;
    AVStream *video_stream;
    AVStream *audio_stream;
    AVCodecContext *video_avctx;
    AVCodecContext *audio_avctx;
    AVFrame *video_frame;
    double video_clock; ///<pts of last decoded frame / predicted pts of next decoded frame
    double video_current_pts; ///<current displayed pts (different from video_clock if frame fifos are used)
    int64_t video_current_pts_time;  ///<time (av_gettime) at which we updated video_current_pts - used to have running video pts
    double frame_timer;
    double frame_last_pts;
    double frame_last_delay;

    AVFrame *audio_frame;
    AVPacket audio_pkt;
    //uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    uint8_t *audio_buf;
    unsigned int audio_buf_size;
    unsigned int audio_buf_index;
    uint8_t *audio_pkt_data;
    int audio_pkt_size;
    AudioParams audio_hw_params_tgt;
    AudioParams audio_hw_params_src;
    struct SwrContext *swr_ctx;
    int av_sync_type;
    double audio_clock;
    double audio_diff_cum; /* used for AV difference average computation */
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    int64_t external_clock_start;

    FFVADisplay *display;
    struct vaapi_context va_context;
    VAProfile *va_profiles;
    uint32_t num_va_profiles;
    FFVASurface *va_surfaces;
    uint32_t num_va_surfaces;
    FFVASurface **va_surfaces_queue;
    uint32_t va_surfaces_queue_length;
    uint32_t va_surfaces_queue_head;
    uint32_t va_surfaces_queue_tail;

    volatile uint32_t state;
    FFVADecoderFrame decoded_frame;

    SDL_Thread *parse_tid;
    SDL_Thread *video_tid;
    PacketQueue audioq;
    PacketQueue videoq;
    int quit;

    FFVADecoderFrame    pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int             pictq_size, pictq_rindex, pictq_windex;
    SDL_mutex       *pictq_mutex;
    SDL_cond        *pictq_cond;
};

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pkt1;

    if(av_dup_packet(pkt) < 0) {
        return -1;
    }
    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;


    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 1;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for(;;) {
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

/* ------------------------------------------------------------------------ */
/* --- VA-API Decoder                                                   --- */
/* ------------------------------------------------------------------------ */

// Ensures the array of VA profiles is allocated and filled up correctly
static int
vaapi_ensure_profiles(FFVADecoder *dec)
{
    struct vaapi_context * const vactx = &dec->va_context;
    VAProfile *profiles;
    int num_profiles;
    VAStatus va_status;

    if (dec->va_profiles && dec->num_va_profiles > 0)
        return 0;

    num_profiles = vaMaxNumProfiles(vactx->display);
    profiles = malloc(num_profiles * sizeof(*profiles));
    if (!profiles)
        return AVERROR(ENOMEM);

    va_status = vaQueryConfigProfiles(vactx->display, profiles, &num_profiles);
    if (!va_check_status(va_status, "vaQueryConfigProfiles()"))
        goto error_query_profiles;

    dec->va_profiles = profiles;
    dec->num_va_profiles = num_profiles;
    return 0;

    /* ERRORS */
error_query_profiles:
    av_log(dec, AV_LOG_ERROR, "failed to query the set of supported profiles\n");
    free(profiles);
    return vaapi_to_ffmpeg_error(va_status);
}

// Ensures the array of VA surfaces and queue of free VA surfaces are allocated
static int
vaapi_ensure_surfaces(FFVADecoder *dec, uint32_t num_surfaces)
{
    uint32_t i, size, new_size;
    void *mem;

    size = dec->num_va_surfaces * sizeof(*dec->va_surfaces);
    new_size = num_surfaces * sizeof(*dec->va_surfaces);
    mem = av_fast_realloc(dec->va_surfaces, &size, new_size);
    if (!mem)
        goto error_alloc_surfaces;
    dec->va_surfaces = mem;

    if (dec->num_va_surfaces < num_surfaces) {
        for (i = dec->num_va_surfaces; i < num_surfaces; i++)
            ffva_surface_init_defaults(&dec->va_surfaces[i]);
        dec->num_va_surfaces = num_surfaces;
    }

    size = dec->va_surfaces_queue_length * sizeof(*dec->va_surfaces_queue);
    new_size = num_surfaces * sizeof(*dec->va_surfaces_queue);
    mem = av_fast_realloc(dec->va_surfaces_queue, &size, new_size);
    if (!mem)
        goto error_alloc_surfaces_queue;
    dec->va_surfaces_queue = mem;

    if (dec->va_surfaces_queue_length < num_surfaces) {
        for (i = dec->va_surfaces_queue_length; i < num_surfaces; i++)
            dec->va_surfaces_queue[i] = NULL;
        dec->va_surfaces_queue_length = num_surfaces;
    }
    return 0;

    /* ERRORS */
error_alloc_surfaces:
    av_log(dec, AV_LOG_ERROR, "failed to allocate VA surfaces array\n");
    return AVERROR(ENOMEM);
error_alloc_surfaces_queue:
    av_log(dec, AV_LOG_ERROR, "failed to allocate VA surfaces queue\n");
    return AVERROR(ENOMEM);
}

// Acquires a surface from the queue of free VA surfaces
static int
vaapi_acquire_surface(FFVADecoder *dec, FFVASurface **out_surface_ptr)
{
    FFVASurface *surface;

    surface = dec->va_surfaces_queue[dec->va_surfaces_queue_head];
    if (!surface)
        return AVERROR_BUG;

    dec->va_surfaces_queue[dec->va_surfaces_queue_head] = NULL;
    dec->va_surfaces_queue_head = (dec->va_surfaces_queue_head + 1) %
        dec->va_surfaces_queue_length;

    if (out_surface_ptr)
        *out_surface_ptr = surface;
    return 0;
}

// Releases a surface back to the queue of free VA surfaces
static int
vaapi_release_surface(FFVADecoder *dec, FFVASurface *s)
{
    FFVASurface * const surface_in_queue =
        dec->va_surfaces_queue[dec->va_surfaces_queue_tail];

    if (surface_in_queue)
        return AVERROR_BUG;

    dec->va_surfaces_queue[dec->va_surfaces_queue_tail] = s;
    dec->va_surfaces_queue_tail = (dec->va_surfaces_queue_tail + 1) %
        dec->va_surfaces_queue_length;
    return 0;
}

// Checks whether the supplied config, i.e. (profile, entrypoint) pair, exists
static bool
vaapi_has_config(FFVADecoder *dec, VAProfile profile, VAEntrypoint entrypoint)
{
    uint32_t i;

    if (vaapi_ensure_profiles(dec) != 0)
        return false;

    for (i = 0; i < dec->num_va_profiles; i++) {
        if (dec->va_profiles[i] == profile)
            break;
    }

    if (i == dec->num_va_profiles)
        return false;
    return true;
}

// Initializes VA decoder comprising of VA config, surfaces and context
static int
vaapi_init_decoder(FFVADecoder *dec, VAProfile profile, VAEntrypoint entrypoint)
{
    AVCodecContext * const avctx = dec->video_avctx;
    struct vaapi_context * const vactx = &dec->va_context;
    VAConfigID va_config = VA_INVALID_ID;
    VAContextID va_context = VA_INVALID_ID;
    VAConfigAttrib va_attribs[1], *va_attrib;
    uint32_t i, num_va_attribs = 0;
    VASurfaceID *va_surfaces = NULL;
    VAStatus va_status;
    int ret;

    va_attrib = &va_attribs[num_va_attribs++];
    va_attrib->type = VAConfigAttribRTFormat;
    va_status = vaGetConfigAttributes(vactx->display, profile, entrypoint,
        va_attribs, num_va_attribs);
    if (!va_check_status(va_status, "vaGetConfigAttributes()"))
        return vaapi_to_ffmpeg_error(va_status);

    va_attrib = &va_attribs[0];
    if (va_attrib->value == VA_ATTRIB_NOT_SUPPORTED ||
        !(va_attrib->value & VA_RT_FORMAT_YUV420))
        goto error_unsupported_chroma_format;
    va_attrib->value = VA_RT_FORMAT_YUV420;

    va_status = vaCreateConfig(vactx->display, profile, entrypoint,
        va_attribs, num_va_attribs, &va_config);
    if (!va_check_status(va_status, "vaCreateConfig()"))
        return vaapi_to_ffmpeg_error(va_status);

    static const int SCRATCH_SURFACES = 4;
    ret = vaapi_ensure_surfaces(dec, avctx->refs + 1 + SCRATCH_SURFACES);
    if (ret != 0)
        goto error_cleanup;

    va_surfaces = malloc(dec->num_va_surfaces * sizeof(*va_surfaces));
    if (!va_surfaces)
        goto error_cleanup;

    va_status = vaCreateSurfaces(vactx->display,
        avctx->coded_width, avctx->coded_height, VA_RT_FORMAT_YUV420,
        dec->num_va_surfaces, va_surfaces);
    if (!va_check_status(va_status, "vaCreateSurfaces()"))
        goto error_cleanup;

    for (i = 0; i < dec->num_va_surfaces; i++) {
        FFVASurface * const s = &dec->va_surfaces[i];
        ffva_surface_init(s, va_surfaces[i], VA_RT_FORMAT_YUV420,
            avctx->coded_width, avctx->coded_height);
        dec->va_surfaces_queue[i] = s;
    }
    dec->va_surfaces_queue_head = 0;
    dec->va_surfaces_queue_tail = 0;

    va_status = vaCreateContext(vactx->display, va_config,
        avctx->coded_width, avctx->coded_height, VA_PROGRESSIVE,
        va_surfaces, dec->num_va_surfaces, &va_context);
    if (!va_check_status(va_status, "vaCreateContext()"))
        goto error_cleanup;

    vactx->config_id = va_config;
    vactx->context_id = va_context;
    free(va_surfaces);
    return 0;

    /* ERRORS */
error_unsupported_chroma_format:
    av_log(dec, AV_LOG_ERROR, "unsupported YUV 4:2:0 chroma format\n");
    return AVERROR(ENOTSUP);
error_cleanup:
    va_destroy_context(vactx->display, &va_context);
    va_destroy_config(vactx->display, &va_config);
    if (ret == 0)
        ret = vaapi_to_ffmpeg_error(va_status);
    free(va_surfaces);
    return ret;
}

// Assigns a VA surface to the supplied AVFrame
static inline void
vaapi_set_frame_surface(AVCodecContext *avctx, AVFrame *frame, FFVASurface *s)
{
#if AV_NUM_DATA_POINTERS > 4
    frame->data[5] = (uint8_t *)s;
#endif
}

// Returns the VA surface object from an AVFrame
static FFVASurface *
vaapi_get_frame_surface(AVCodecContext *avctx, AVFrame *frame)
{
#if AV_NUM_DATA_POINTERS > 4
    return (FFVASurface *)frame->data[5];
#else
    FFVADecoder * const dec = avctx->opaque;
    VASurfaceID va_surface;
    uint32_t i;

    va_surface = (uintptr_t)frame->data[3];
    if (va_surface == VA_INVALID_ID)
        return NULL;

    for (i = 0; i < dec->num_va_surfaces; i++) {
        FFVASurface * const s = &dec->va_surfaces[i];
        if (s->id == va_surface)
            return s;
    }
    return NULL;
#endif
}

// AVCodecContext.get_format() implementation for VA-API
static enum AVPixelFormat
vaapi_get_format(AVCodecContext *avctx, const enum AVPixelFormat *pix_fmts)
{
    FFVADecoder * const dec = avctx->opaque;
    VAProfile profiles[3];
    uint32_t i, num_profiles;

    // Find a VA format
    for (i = 0; pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
        if (pix_fmts[i] == AV_PIX_FMT_VAAPI)
            break;
    }
    if (pix_fmts[i] == AV_PIX_FMT_NONE)
        return AV_PIX_FMT_NONE;

    // Find a suitable VA profile that fits FFmpeg config
    num_profiles = 0;
    if (!ffmpeg_to_vaapi_profile(avctx->codec_id, avctx->profile,
            &profiles[num_profiles]))
        return AV_PIX_FMT_NONE;

    switch (profiles[num_profiles++]) {
    case VAProfileMPEG2Simple:
        profiles[num_profiles++] = VAProfileMPEG2Main;
        break;
    case VAProfileMPEG4Simple:
        profiles[num_profiles++] = VAProfileMPEG4AdvancedSimple;
        // fall-through
    case VAProfileMPEG4AdvancedSimple:
        profiles[num_profiles++] = VAProfileMPEG4Main;
        break;
#if 0
    case VAProfileH264Baseline:
        profiles[num_profiles++] = VAProfileH264Main;
        break;
#endif
    case VAProfileH264ConstrainedBaseline:
        profiles[num_profiles++] = VAProfileH264Main;
        // fall-through
    case VAProfileH264Main:
        profiles[num_profiles++] = VAProfileH264High;
        break;
    case VAProfileVC1Simple:
        profiles[num_profiles++] = VAProfileVC1Main;
        // fall-through
    case VAProfileVC1Main:
        profiles[num_profiles++] = VAProfileVC1Advanced;
        break;
    default:
        break;
    }
    for (i = 0; i < num_profiles; i++) {
        if (vaapi_has_config(dec, profiles[i], VAEntrypointVLD))
            break;
    }
    if (i == num_profiles)
        return AV_PIX_FMT_NONE;
    if (vaapi_init_decoder(dec, profiles[i], VAEntrypointVLD) < 0)
        return AV_PIX_FMT_NONE;
    return AV_PIX_FMT_VAAPI;
}

// Common initialization of AVFrame fields for VA-API purposes
static void
vaapi_get_buffer_common(AVCodecContext *avctx, AVFrame *frame, FFVASurface *s)
{
    memset(frame->data, 0, sizeof(frame->data));
    frame->data[0] = (uint8_t *)(uintptr_t)s->id;
    frame->data[3] = (uint8_t *)(uintptr_t)s->id;
    memset(frame->linesize, 0, sizeof(frame->linesize));
    frame->linesize[0] = avctx->coded_width; /* XXX: 8-bit per sample only */
    vaapi_set_frame_surface(avctx, frame, s);
}

#if AV_FEATURE_AVFRAME_REF
// AVCodecContext.get_buffer2() implementation for VA-API
static int
vaapi_get_buffer2(AVCodecContext *avctx, AVFrame *frame, int flags)
{
    FFVADecoder * const dec = avctx->opaque;
    FFVASurface *s;
    AVBufferRef *buf;
    int ret;

    if (!(avctx->codec->capabilities & CODEC_CAP_DR1))
        return avcodec_default_get_buffer2(avctx, frame, flags);

    ret = vaapi_acquire_surface(dec, &s);
    if (ret != 0)
        return ret;

    buf = av_buffer_create((uint8_t *)s, 0,
        (void (*)(void *, uint8_t *))vaapi_release_surface, dec,
        AV_BUFFER_FLAG_READONLY);
    if (!buf) {
        vaapi_release_surface(dec, s);
        return AVERROR(ENOMEM);
    }
    frame->buf[0] = buf;

    vaapi_get_buffer_common(avctx, frame, s);
    return 0;
}
#else
// AVCodecContext.get_buffer() implementation for VA-API
static int
vaapi_get_buffer(AVCodecContext *avctx, AVFrame *frame)
{
    FFVADecoder * const dec = avctx->opaque;
    FFVASurface *s;
    int ret;

    ret = vaapi_acquire_surface(dec, &s);
    if (ret != 0)
        return ret;

    frame->opaque = NULL;
    frame->type = FF_BUFFER_TYPE_USER;
    frame->reordered_opaque = avctx->reordered_opaque;

    vaapi_get_buffer_common(avctx, frame, s);
    return 0;
}

// AVCodecContext.reget_buffer() implementation for VA-API
static int
vaapi_reget_buffer(AVCodecContext *avctx, AVFrame *frame)
{
    assert(0 && "FIXME: implement AVCodecContext::reget_buffer() [VA-API]");

    // XXX: this is not the correct implementation
    return avcodec_default_reget_buffer(avctx, frame);
}

// AVCodecContext.release_buffer() implementation for VA-API
static void
vaapi_release_buffer(AVCodecContext *avctx, AVFrame *frame)
{
    FFVADecoder * const dec = avctx->opaque;
    FFVASurface * const s = vaapi_get_frame_surface(avctx, frame);

    memset(frame->data, 0, sizeof(frame->data));
    if (s && vaapi_release_surface(dec, s) != 0)
        return;
}
#endif

// Initializes AVCodecContext for VA-API decoding purposes
static void
vaapi_init_context(FFVADecoder *dec)
{
    AVCodecContext * const avctx = dec->video_avctx;

    avctx->hwaccel_context = &dec->va_context;
    avctx->thread_count = 1;
    avctx->draw_horiz_band = 0;
    avctx->slice_flags = SLICE_FLAG_CODED_ORDER|SLICE_FLAG_ALLOW_FIELD;

    avctx->get_format = vaapi_get_format;
#if AV_FEATURE_AVFRAME_REF
    avctx->get_buffer2 = vaapi_get_buffer2;
#else
    avctx->get_buffer = vaapi_get_buffer;
    avctx->reget_buffer = vaapi_reget_buffer;
    avctx->release_buffer = vaapi_release_buffer;
#endif
}

// Initializes decoder for VA-API purposes, e.g. creates the VA display
static void
vaapi_init(FFVADecoder *dec)
{
    struct vaapi_context * const vactx = &dec->va_context;

    memset(vactx, 0, sizeof(*vactx));
    vactx->config_id = VA_INVALID_ID;
    vactx->context_id = VA_INVALID_ID;
    vactx->display = dec->display->va_display;
}

// Destroys all VA-API related resources
static void
vaapi_finalize(FFVADecoder *dec)
{
    struct vaapi_context * const vactx = &dec->va_context;
    uint32_t i;

    if (vactx->display) {
        va_destroy_context(vactx->display, &vactx->context_id);
        va_destroy_config(vactx->display, &vactx->config_id);
        if (dec->va_surfaces) {
            for (i = 0; i < dec->num_va_surfaces; i++)
                va_destroy_surface(vactx->display, &dec->va_surfaces[i].id);
        }
    }
    free(dec->va_surfaces);
    dec->num_va_surfaces = 0;
    free(dec->va_surfaces_queue);
    dec->va_surfaces_queue_length = 0;
    free(dec->va_profiles);
    dec->num_va_profiles = 0;
}

/* ------------------------------------------------------------------------ */
/* --- Base Decoder (SW)                                                --- */
/* ------------------------------------------------------------------------ */

static const AVClass *
ffva_decoder_class(void)
{
    static const AVClass g_class = {
        .class_name     = "FFVADecoder",
        .item_name      = av_default_item_name,
        .option         = NULL,
        .version        = LIBAVUTIL_VERSION_INT,
    };
    return &g_class;
}

static void
decoder_init_context(FFVADecoder *dec, AVCodecContext *avctx)
{
    dec->video_avctx = avctx;
    avctx->opaque = dec;
    vaapi_init_context(dec);
}

static int
decoder_init(FFVADecoder *dec, FFVADisplay *display)
{
    dec->klass = ffva_decoder_class();
    av_register_all();

    dec->display = display;
    vaapi_init(dec);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        av_log(dec, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }

    dec->state = STATE_INITIALIZED;
    return 0;
}

static void
decoder_finalize(FFVADecoder *dec)
{
    ffva_decoder_close(dec);
    vaapi_finalize(dec);
}

int resample(FFVADecoder *dec)
{
    AVFrame *af = dec->audio_frame;;
	int data_size = 0;
	int resampled_data_size = 0;
	int64_t dec_channel_layout;

	data_size = av_samples_get_buffer_size(NULL,
			av_frame_get_channels(af),
			af->nb_samples,
			af->format,
			1);
    dec_channel_layout =
        (af->channel_layout && av_frame_get_channels(af) == av_get_channel_layout_nb_channels(af->channel_layout)) ?
        af->channel_layout : av_get_default_channel_layout(av_frame_get_channels(af));
	if( 	af->format 		 	!= dec->audio_hw_params_src.fmt 				||
			af->sample_rate 	!= dec->audio_hw_params_src.freq 	 		||
			dec_channel_layout 	!= dec->audio_hw_params_src.channel_layout 	||
			!dec->swr_ctx) {
		swr_free(&dec->swr_ctx);
		dec->swr_ctx = swr_alloc_set_opts(dec->swr_ctx,
										dec->audio_hw_params_tgt.channel_layout, dec->audio_hw_params_tgt.fmt, dec->audio_hw_params_tgt.freq,
										dec_channel_layout, af->format, af->sample_rate,
										0, NULL);
        if (!dec->swr_ctx || swr_init(dec->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    af->sample_rate, av_get_sample_fmt_name(af->format), av_frame_get_channels(af),
                    dec->audio_hw_params_tgt.freq, av_get_sample_fmt_name(dec->audio_hw_params_tgt.fmt), dec->audio_hw_params_tgt.channels);
            swr_free(&dec->swr_ctx);
            return -1;
        }
		dec->audio_hw_params_src.channels = av_frame_get_channels(af);
		dec->audio_hw_params_src.fmt = af->format;
		dec->audio_hw_params_src.freq = af->sample_rate;
	}

    if (dec->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->extended_data;
        uint8_t **out = &dec->audio_buf;
        int out_count = (int64_t)af->nb_samples * dec->audio_hw_params_tgt.freq / af->sample_rate + 256;
        int out_size  = av_samples_get_buffer_size(NULL, dec->audio_hw_params_tgt.channels, out_count, dec->audio_hw_params_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        av_fast_malloc(&dec->audio_buf, &dec->audio_buf_size, out_size);
        if (!dec->audio_buf)
            return AVERROR(ENOMEM);
        len2 = swr_convert(dec->swr_ctx, out, out_count, in, af->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(dec->swr_ctx) < 0)
                swr_free(&dec->swr_ctx);
        }
        resampled_data_size = len2 * dec->audio_hw_params_tgt.channels * av_get_bytes_per_sample(dec->audio_hw_params_tgt.fmt);
    } else {
        dec->audio_buf = af->data[0];
        resampled_data_size = data_size;
    }

    return resampled_data_size;
}

int audio_decode_frame(FFVADecoder *dec, double *pts_ptr)
{
	int len1, data_size = 0;
    AVPacket *pkt = &dec->audio_pkt;
    double pts;
    int n;

	for(;;) {
		while(dec->audio_pkt_size > 0) {
			int got_frame = 0;
			len1 = avcodec_decode_audio4(dec->audio_avctx, dec->audio_frame, &got_frame, pkt);
			if(len1 < 0) {
				/* if error, skip frame */
				dec->audio_pkt_size = 0;
				break;
			}
			dec->audio_pkt_data += len1;
			dec->audio_pkt_size -= len1;
			data_size = 0;
			if(got_frame) {
				data_size = resample(dec);
                //data_size = av_samples_get_buffer_size(NULL,
				// 		dec->audio_avctx->channels,
				// 		dec->audio_frame->nb_samples,
				// 		dec->audio_avctx->sample_fmt,
				// 		1);
				assert(data_size <= dec->audio_buf_size);
				//memcpy(dec->audio_buf, dec->audio_frame->data[0], data_size);
			}
			if(data_size <= 0) {
				/* No data yet, get more frames */
				continue;
			}
			// memcpy(dec->audio_buf, frame.data[0], data_size);
            pts = dec->audio_clock;
            *pts_ptr = pts;
            n = 2 * dec->audio_avctx->channels;
            dec->audio_clock += (double)data_size /
                (double)(n * dec->audio_avctx->sample_rate);
			/* We have data, return it and come back for more later */
            //av_log(dec, AV_LOG_DEBUG, "audio clk1=%lf\n", dec->audio_clock);
			return data_size;
		}
		if(pkt->data)
			av_free_packet(pkt);

        if(dec->quit)
            return -1;

		if(packet_queue_get(&dec->audioq, pkt, 1) < 0)
			return -1;

		dec->audio_pkt_data = pkt->data;
		dec->audio_pkt_size = pkt->size;
        /* if update, update the audio clock w/pts */
        if(pkt->pts != AV_NOPTS_VALUE) {
            dec->audio_clock = av_q2d(dec->audio_stream->time_base)*pkt->pts;
            //av_log(dec, AV_LOG_DEBUG, "audio clk2=%lf\n", dec->audio_clock);
        }
    }
}

double get_audio_clock(FFVADecoder *dec) {
    double pts;
    int hw_buf_size, bytes_per_sec, n;

    pts = dec->audio_clock; /* maintained in the audio thread */
    hw_buf_size = dec->audio_buf_size - dec->audio_buf_index;
    bytes_per_sec = 0;
    n = dec->audio_avctx->channels * 2;
    if(dec->audio_stream) {
        bytes_per_sec = dec->audio_avctx->sample_rate * n;
    }
    if(bytes_per_sec) {
        pts -= (double)hw_buf_size / bytes_per_sec;
    }
    //av_log(dec, AV_LOG_INFO, "get audio clk=%lf\n", pts);
    return pts;
}

double get_video_clock(FFVADecoder *dec) {
    double delta;

    delta = (av_gettime_relative() - dec->video_current_pts_time) / 1000000.0;
    return dec->video_current_pts + delta;
}

double get_external_clock(FFVADecoder *dec) {
    return (av_gettime_relative() - dec->external_clock_start) / 1000000.0;
}

double get_master_clock(FFVADecoder *dec) {
    if(dec->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        return get_video_clock(dec);
    } else if(dec->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        return get_audio_clock(dec);
    } else {
        return get_external_clock(dec);
    }
}

/* Add or subtract samples to get a better sync, return new
   audio buffer size */
int synchronize_audio(FFVADecoder *dec, short *samples,
        int samples_size, double pts)
{
    int n;
    double ref_clock;

    n = 2 * dec->audio_avctx->channels;

    if(dec->av_sync_type != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int wanted_size, min_size, max_size /*, nb_samples */;

        ref_clock = get_master_clock(dec);

        diff = get_audio_clock(dec) - ref_clock;

        //av_log(dec, AV_LOG_INFO, "ref clk=%lf diff=%lf\n", ref_clock, diff);
        if(diff < AV_NOSYNC_THRESHOLD) {
            // accumulate the diffs
            dec->audio_diff_cum = diff + dec->audio_diff_avg_coef * dec->audio_diff_cum;
            if(dec->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                dec->audio_diff_avg_count++;
            } else {
                avg_diff = dec->audio_diff_cum * (1.0 - dec->audio_diff_avg_coef);
                //av_log(dec, AV_LOG_DEBUG, "avg_diff=%lf\n", avg_diff);
                if(fabs(avg_diff) >= dec->audio_diff_threshold) {
                    wanted_size = samples_size + ((int)(diff * dec->audio_avctx->sample_rate) * n);
                    //min_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
                    //max_size = samples_size * ((100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100);
                    min_size = 0;
                    max_size = samples_size;
                    //av_log(NULL, AV_LOG_DEBUG, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                    //        diff, avg_diff, wanted_size - samples_size,
                    //        dec->audio_clock, dec->audio_diff_threshold);
                    if(wanted_size < min_size) {
                        wanted_size = min_size;
                    } else if (wanted_size > max_size) {
                        wanted_size = max_size;
                    }
                    if(wanted_size < samples_size) {
                        /* remove samples */
                        samples_size = wanted_size;
                        //av_log(dec, AV_LOG_DEBUG, "remove samples size=%d\n", samples_size);
                    } else if(wanted_size > samples_size) {
                        uint8_t *samples_end, *q;
                        int nb;

                        /* add samples by copying final sample*/
                        nb = (samples_size - wanted_size);
                        samples_end = (uint8_t *)samples + samples_size - n;
                        q = samples_end + n;
                        while(nb > 0) {
                            memcpy(q, samples_end, n);
                            q += n;
                            nb -= n;
                        }
                        samples_size = wanted_size;
                        //av_log(dec, AV_LOG_DEBUG, "add samples size=%d\n", samples_size);
                    }
                }
            }
        } else {
            /* difference dec TOO big; reset diff stuff */
            //av_log(dec, AV_LOG_DEBUG, "audio clk=%lf ref clk=%lf diff=%lf\n", get_audio_clock(dec), ref_clock, diff);
            dec->audio_diff_avg_count = 0;
            dec->audio_diff_cum = 0;
        }
    }
    return samples_size;
}

void audio_callback(void *userdata, Uint8 *stream, int len)
{
    FFVADecoder *dec = (FFVADecoder *)userdata;
    int len1, audio_size;
    double pts;

    while(len > 0) {
        if(dec->audio_buf_index >= dec->audio_buf_size) {
            /* We have already sent all our data; get more */
            audio_size = audio_decode_frame(dec, &pts);
            if(audio_size < 0) {
                /* If error, output silence */
                dec->audio_buf_size = 1024; // arbitrary?
                memset(dec->audio_buf, 0, dec->audio_buf_size);
            } else {
                audio_size = synchronize_audio(dec, (int16_t *)dec->audio_buf, audio_size, pts);
                //audio_size = synchronize_audio(dec, audio_size);
                dec->audio_buf_size = audio_size;
            }
            dec->audio_buf_index = 0;
        }
        len1 = dec->audio_buf_size - dec->audio_buf_index;
        if(len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)dec->audio_buf + dec->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        dec->audio_buf_index += len1;
    }
}

static int audio_decoder_open(FFVADecoder *dec)
{
    AVCodecContext *avctx;
    AVCodec *codec;
    SDL_AudioSpec wanted_spec, spec;
    int64_t channel_layout;

    avctx = dec->audio_avctx;
    if (avctx)
        codec = avcodec_find_decoder(avctx->codec_id);
    if (!codec) {
        av_log(dec, AV_LOG_ERROR, "failed to find codec info for audio codec %d\n",
                avctx->codec_id);
        return -1;
    }

    if (avcodec_open2(avctx, codec, NULL) < 0) {
        av_log(dec, AV_LOG_ERROR, "failed to open codec %d\n", avctx->codec_id);
        return -1;
    }

    wanted_spec.channels = avctx->channels;//av_get_channel_layout_nb_channels(channel_layout);
    wanted_spec.freq = avctx->sample_rate;
    //av_log(dec, AV_LOG_DEBUG, "channels=%d\n", wanted_spec.channels);
    //av_log(dec, AV_LOG_DEBUG, "sample_rate=%d\n", wanted_spec.freq);
    //av_log(dec, AV_LOG_DEBUG, "channel_layout=%ld\n", avctx->channel_layout);

    if (wanted_spec.freq < 0 || wanted_spec.channels < 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    //wanted_spec.samples = 1024;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = dec;

    //av_log(dec, AV_LOG_INFO, "audio spec samples=%d\n", wanted_spec.samples);
    if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        av_log(dec, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n", 
                wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        return -1;
    }

    dec->audio_hw_params_tgt.fmt = AV_SAMPLE_FMT_S16;
    dec->audio_hw_params_tgt.freq = spec.freq;
    dec->audio_hw_params_tgt.channel_layout = avctx->channel_layout;
    dec->audio_hw_params_tgt.channels =  spec.channels;
    dec->audio_hw_params_tgt.frame_size = av_samples_get_buffer_size(NULL, dec->audio_hw_params_tgt.channels, 1, dec->audio_hw_params_tgt.fmt, 1);
    dec->audio_hw_params_tgt.bytes_per_sec = av_samples_get_buffer_size(NULL, dec->audio_hw_params_tgt.channels, dec->audio_hw_params_tgt.freq, dec->audio_hw_params_tgt.fmt, 1);
    if (dec->audio_hw_params_tgt.bytes_per_sec <= 0 || dec->audio_hw_params_tgt.frame_size <= 0) {
        printf("size error\n");
        return -1;
    }
    dec->audio_hw_params_src = dec->audio_hw_params_tgt;
    dec->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
    //dec->audio_diff_threshold = (double)(spec.size) / dec->audio_hw_params_tgt.bytes_per_sec;
    dec->audio_diff_threshold = 0.2;
    //av_log(dec, AV_LOG_DEBUG, "audio_diff_avg_coef=%lf\n", dec->audio_diff_avg_coef);
    //av_log(dec, AV_LOG_DEBUG, "audio_diff_threshold=%lf\n", dec->audio_diff_threshold);
    SDL_PauseAudio(0);
    return 0;
}

static int
decoder_open(FFVADecoder *dec, const char *filename)
{
    AVFormatContext *fmtctx;
    AVCodecContext *avctx;
    AVCodec *codec;
    char errbuf[BUFSIZ];
    int i, ret;

    if (dec->state & STATE_OPENED)
        return 0;

    dec->av_sync_type = DEFAULT_AV_SYNC_TYPE;
    // Open and identify media file
    ret = avformat_open_input(&dec->fmtctx, filename, NULL, NULL);
    if (ret != 0)
        goto error_open_file;

    dec->fmtctx->max_analyze_duration2 = 1000000;
    ret = avformat_find_stream_info(dec->fmtctx, NULL);
    if (ret < 0)
        goto error_identify_file;
    av_dump_format(dec->fmtctx, 0, filename, 0);
    fmtctx = dec->fmtctx;

    // Find the video stream and identify the codec
    for (i = 0; i < fmtctx->nb_streams; i++) {
        if (fmtctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO &&
            !dec->video_stream) {
            dec->video_stream = fmtctx->streams[i];
            avctx = dec->video_stream->codec;

            dec->frame_timer = (double)(av_gettime_relative())/ 1000000.0;
            dec->frame_last_delay = 40e-3;
            dec->video_current_pts_time = av_gettime_relative();

            decoder_init_context(dec, avctx);

            codec = avcodec_find_decoder(avctx->codec_id);
            if (!codec)
                goto error_no_codec;
            ret = avcodec_open2(avctx, codec, NULL);
            if (ret < 0)
                goto error_open_codec;

            dec->video_frame = av_frame_alloc();
            if (!dec->video_frame)
                goto error_alloc_frame;

            packet_queue_init(&dec->videoq);
        }
        else if (fmtctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
            !dec->audio_stream) {
            dec->audio_stream = fmtctx->streams[i];
            dec->audio_avctx = dec->audio_stream->codec;

            dec->audio_frame = av_frame_alloc();
            if (!dec->audio_frame)
                goto error_alloc_frame;

            packet_queue_init(&dec->audioq);

            if (audio_decoder_open(dec) < 0) {
                av_log(dec, AV_LOG_ERROR, "failed to find a audio stream\n");
            }
        }
        else
            fmtctx->streams[i]->discard = AVDISCARD_ALL;
    }

    dec->external_clock_start = av_gettime_relative();

    if (!dec->video_stream)
        goto error_no_video_stream;

    dec->state |= STATE_OPENED;
    return 0;

    /* ERRORS */
error_open_file:
    av_log(dec, AV_LOG_ERROR, "failed to open file `%s': %s\n", filename,
        ffmpeg_strerror(ret, errbuf));
    return ret;
error_identify_file:
    av_log(dec, AV_LOG_ERROR, "failed to identify file `%s': %s\n", filename,
        ffmpeg_strerror(ret, errbuf));
    return ret;
error_no_video_stream:
    av_log(dec, AV_LOG_ERROR, "failed to find a video stream\n");
    return AVERROR_STREAM_NOT_FOUND;
error_no_codec:
    av_log(dec, AV_LOG_ERROR, "failed to find codec info for codec %d\n",
        avctx->codec_id);
    return AVERROR_DECODER_NOT_FOUND;
error_open_codec:
    av_log(dec, AV_LOG_ERROR, "failed to open codec %d\n", avctx->codec_id);
    return ret;
error_alloc_frame:
    av_log(dec, AV_LOG_ERROR, "failed to allocate video frame\n");
    return AVERROR(ENOMEM);
}

static void
decoder_close(FFVADecoder *dec)
{
    ffva_decoder_stop(dec);

    if (dec->video_avctx) {
        avcodec_close(dec->video_avctx);
        dec->video_avctx = NULL;
    }

    if (dec->audio_avctx) {
        avcodec_close(dec->audio_avctx);
        dec->audio_avctx = NULL;
    }

    if (dec->fmtctx) {
        avformat_close_input(&dec->fmtctx);
        dec->fmtctx = NULL;
    }
    av_frame_free(&dec->video_frame);
    av_frame_free(&dec->audio_frame);

    dec->state &= ~STATE_OPENED;
}

double synchronize_video(FFVADecoder *dec, double pts) {
    double frame_delay;

    if(pts != 0) {
        /* if we have pts, set video clock to it */
        dec->video_clock = pts;
    } else {
        /* if we aren't given a pts, set it to the clock */
        pts = dec->video_clock;
    }
    //av_log(dec, AV_LOG_INFO, "video pts=%lf, video_avctx->timebase=%d/%d\n", pts, dec->video_avctx->time_base.num,  dec->video_avctx->time_base.den);
    /* update the video clock */
    frame_delay = av_q2d(dec->video_avctx->time_base);
    /* if we are repeating a frame, adjust clock accordingly */
    frame_delay += dec->video_frame->repeat_pict * (frame_delay * 0.5);
    dec->video_clock += frame_delay;

    return pts;
}

int queue_picture(FFVADecoder *dec, FFVADecoderFrame frame, double pts) {
    SDL_LockMutex(dec->pictq_mutex);
    while(dec->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE &&
            !dec->quit) {
        SDL_CondWait(dec->pictq_cond, dec->pictq_mutex);
    }
    SDL_UnlockMutex(dec->pictq_mutex);

    if(dec->quit)
        return -1;

    frame.pts = pts;
    // windex dec set to 0 initially
    dec->pictq[dec->pictq_windex] = frame;

    /* now we inform our ddecplay thread that we have a pic ready */
    if(++dec->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
        dec->pictq_windex = 0;
    }
    SDL_LockMutex(dec->pictq_mutex);
    dec->pictq_size++;
    SDL_UnlockMutex(dec->pictq_mutex);

    return 0;
}

void video_display(FFVADecoder *dec) {
    FFVADecoderFrame *dec_frame;

    //av_log(dec, AV_LOG_INFO, "video display frame:pictq_rindex=%d\n", dec->pictq_rindex);
    dec_frame = &dec->pictq[dec->pictq_rindex];
    render_frame(dec_frame);
    ffva_decoder_put_frame(dec, dec_frame);
}

void video_refresh_timer(void *userdata) {
    FFVADecoder *dec = (FFVADecoder*) userdata;
    FFVADecoderFrame *vp;
    double actual_delay, delay, sync_threshold, ref_clock, diff;

    if(dec->video_stream) {
        if(dec->pictq_size == 0) {
            schedule_refresh(dec, 1);
        } else {
            vp = &dec->pictq[dec->pictq_rindex];

            dec->video_current_pts = vp->pts;
            dec->video_current_pts_time = av_gettime_relative();
            delay = vp->pts - dec->frame_last_pts; /* the pts from last time */
            //av_log(dec, AV_LOG_DEBUG, "video_current_pts=%lf video_current_pts_time=%ld delay=%lf\n", dec->video_current_pts, dec->video_current_pts_time, delay);
            if(delay <= 0 || delay >= 1.0) {
                /* if incorrect delay, use previous one */
                delay = dec->frame_last_delay;
            }
            /* save for next time */
            dec->frame_last_delay = delay;
            dec->frame_last_pts = vp->pts;

            /* update delay to sync to audio if not master source */
            if(dec->av_sync_type != AV_SYNC_VIDEO_MASTER) {
                ref_clock = get_master_clock(dec);
                diff = vp->pts - ref_clock;

                /* Skip or repeat the frame. Take delay into account
                   FFPlay still doesn't "know if thdec dec the best guess." */
                sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
                if(fabs(diff) < AV_NOSYNC_THRESHOLD) {
                    if(diff <= -sync_threshold) {
                        delay = 0;
                    } else if(diff >= sync_threshold) {
                        delay = 2 * delay;
                    }
                }
            }
            dec->frame_timer += delay;
            /* computer the REAL delay */
            actual_delay = dec->frame_timer - (av_gettime_relative() / 1000000.0);
            //av_log(dec, AV_LOG_DEBUG, "ref_clock=%lf diff=%lf delay=%lf actual_delay=%lf\n", ref_clock, diff, delay, actual_delay);
            if(actual_delay < 0.010) {
                /* Really it should skip the picture instead */
                actual_delay = 0.010;
            }
            schedule_refresh(dec, (int)(actual_delay * 1000 + 0.5));

            /* show the picture! */
            video_display(dec);

            /* update queue for next picture! */
            if(++dec->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
                dec->pictq_rindex = 0;
            }
            SDL_LockMutex(dec->pictq_mutex);
            dec->pictq_size--;
            SDL_CondSignal(dec->pictq_cond);
            SDL_UnlockMutex(dec->pictq_mutex);
        }
    } else {
        schedule_refresh(dec, 100);
    }
}

Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
    SDL_Event event;

    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0; /* 0 means stop timer */
}

/* schedule a video refresh in 'delay' ms */
void schedule_refresh(FFVADecoder *dec, int delay) {
    //av_log(NULL, AV_LOG_INFO, "%s:delay=%dms\n", __func__, delay);
    SDL_AddTimer(delay, sdl_refresh_timer_cb, dec);
}

static int
handle_frame(FFVADecoder *dec, AVFrame *frame)
{
    FFVADecoderFrame * const dec_frame = &dec->decoded_frame;
    VARectangle * const crop_rect = &dec_frame->crop_rect;
    int data_offset;

    dec_frame->frame = frame;
    dec_frame->surface = vaapi_get_frame_surface(dec->video_avctx, frame);
    if (!dec_frame->surface)
        return AVERROR(EFAULT);

    data_offset = frame->data[0] - frame->data[3];
    dec_frame->has_crop_rect = data_offset > 0   ||
        frame->width  != dec->video_avctx->coded_width ||
        frame->height != dec->video_avctx->coded_height;
    crop_rect->x = data_offset % frame->linesize[0];
    crop_rect->y = data_offset / frame->linesize[0];
    crop_rect->width = frame->width;
    crop_rect->height = frame->height;
    return 0;
}

static int
decode_packet(FFVADecoder *dec, AVPacket *packet, int *got_frame_ptr)
{
    char errbuf[BUFSIZ];
    int got_frame, ret;
    double pts;

    if (!got_frame_ptr)
        got_frame_ptr = &got_frame;

    ret = avcodec_decode_video2(dec->video_avctx, dec->video_frame, got_frame_ptr, packet);
    if (ret < 0)
        goto error_decode_frame;

    if (*got_frame_ptr)
        return handle_frame(dec, dec->video_frame);
    return AVERROR(EAGAIN);

    /* ERRORS */
error_decode_frame:
    av_log(dec, AV_LOG_ERROR, "failed to decode frame: %s\n",
        ffmpeg_strerror(ret, errbuf));
    return ret;
}

static int
decoder_run(FFVADecoder *dec)
{
    AVPacket packet;
    char errbuf[BUFSIZ];
    int got_frame, ret;

    av_init_packet(&packet);
    packet.data = NULL;
    packet.size = 0;

    do {
        // Read frame from file
        ret = av_read_frame(dec->fmtctx, &packet);
        if (ret == AVERROR_EOF)
            break;
        else if (ret < 0)
            goto error_read_frame;

        // Decode video packet
        if (packet.stream_index == dec->video_stream->index) {
            ret = decode_packet(dec, &packet, NULL);
            av_free_packet(&packet);
        }
        else if (packet.stream_index == dec->audio_stream->index) {
            ret = packet_queue_put(&dec->audioq, &packet);
        }
        else
            ret = AVERROR(EAGAIN);
    } while (ret == AVERROR(EAGAIN));
    if (ret == 0)
        return 0;

    // Decode cached frames
    if (packet.stream_index == dec->video_stream->index) {
        packet.data = NULL;
        packet.size = 0;
        ret = decode_packet(dec, &packet, &got_frame);
        if (ret == AVERROR(EAGAIN) && !got_frame)
            ret = AVERROR_EOF;
    }
    return ret;


    /* ERRORS */
error_read_frame:
    av_log(dec, AV_LOG_ERROR, "failed to read frame: %s\n",
        ffmpeg_strerror(ret, errbuf));
    return ret;
}

static int
decoder_flush(FFVADecoder *dec)
{
    return 0;
}

static int
decoder_start(FFVADecoder *dec)
{
    if (dec->state & STATE_STARTED)
        return 0;
    if (!(dec->state & STATE_OPENED))
        return AVERROR_UNKNOWN;

    dec->state |= STATE_STARTED;
    return 0;
}

static int
decoder_stop(FFVADecoder *dec)
{
    if (!(dec->state & STATE_STARTED))
        return 0;

    dec->state &= ~STATE_STARTED;
    return 0;
}

static int
decoder_get_frame(FFVADecoder *dec, FFVADecoderFrame **out_frame_ptr)
{
    FFVADecoderFrame *frame = NULL;
    int ret;

    if (!(dec->state & STATE_STARTED)) {
        ret = decoder_start(dec);
        if (ret < 0)
            return ret;
    }

    ret = decoder_run(dec);
    if (ret == 0)
        frame = &dec->decoded_frame;

    if (out_frame_ptr)
        *out_frame_ptr = frame;
    return ret;
}

static void
decoder_put_frame(FFVADecoder *dec, FFVADecoderFrame *frame)
{
    if (!frame || frame != &dec->decoded_frame)
        return;
}

/* ------------------------------------------------------------------------ */
/* --- Interface                                                        --- */
/* ------------------------------------------------------------------------ */

// Creates a new decoder instance
FFVADecoder *
ffva_decoder_new(FFVADisplay *display)
{
    FFVADecoder *dec;

    if (!display)
        return NULL;

    dec = calloc(1, sizeof(*dec));
    if (!dec)
        return NULL;
    if (decoder_init(dec, display) != 0)
        goto error;
    return dec;

error:
    ffva_decoder_free(dec);
    return NULL;
}

// Destroys the supplied decoder instance
void
ffva_decoder_free(FFVADecoder *dec)
{
    if (!dec)
        return;
    decoder_finalize(dec);
    free(dec);
}

/// Releases decoder instance and resets the supplied pointer to NULL
void
ffva_decoder_freep(FFVADecoder **dec_ptr)
{
    if (!dec_ptr)
        return;
    ffva_decoder_free(*dec_ptr);
    *dec_ptr = NULL;
}

// Initializes the decoder instance for the supplied video file by name
int
ffva_decoder_open(FFVADecoder *dec, const char *filename)
{
    if (!dec || !filename)
        return AVERROR(EINVAL);
    return decoder_open(dec, filename);
}

// Destroys the decoder resources used for processing the previous file
void
ffva_decoder_close(FFVADecoder *dec)
{
    if (!dec)
        return;
    decoder_close(dec);
}

// Starts processing the video file that was previously opened
int
ffva_decoder_start(FFVADecoder *dec)
{
    if (!dec)
        return AVERROR(EINVAL);
    return decoder_start(dec);
}

// Stops processing the active video file
void
ffva_decoder_stop(FFVADecoder *dec)
{
    if (!dec)
        return;
    decoder_stop(dec);
}

// Flushes any source data to be decoded
int
ffva_decoder_flush(FFVADecoder *dec)
{
    if (!dec)
        return AVERROR(EINVAL);
    return decoder_flush(dec);
}

// Returns some media info from an opened file
bool
ffva_decoder_get_info(FFVADecoder *dec, FFVADecoderInfo *info)
{
    if (!dec || !info)
        return false;
    if (!dec->video_avctx)
        return false;

    AVCodecContext * const avctx = dec->video_avctx;
    info->codec         = avctx->codec_id;
    info->profile       = avctx->profile;
    info->width         = avctx->width;
    info->height        = avctx->height;
    return true;
}

// Acquires the next decoded frame
int
ffva_decoder_get_frame(FFVADecoder *dec, FFVADecoderFrame **out_frame_ptr)
{
    if (!dec || !out_frame_ptr)
        return AVERROR(EINVAL);
    return decoder_get_frame(dec, out_frame_ptr);
}

// Releases the decoded frame back to the decoder for future use
void
ffva_decoder_put_frame(FFVADecoder *dec, FFVADecoderFrame *frame)
{
    if (!dec || !frame)
        return;
    decoder_put_frame(dec, frame);
}

int parse_thread(void *arg)
{
    FFVADecoder *dec = arg;
    AVPacket packet;

    av_init_packet(&packet);
    packet.data = NULL;
    packet.size = 0;

    for(;;) {
        if(dec->quit) {
            break;
        }
        // seek stuff goes here
        //if(dec->audioq.size > MAX_AUDIOQ_SIZE ||
        //        dec->videoq.size > MAX_VIDEOQ_SIZE) {
        //    av_log(dec, AV_LOG_INFO, "delay 10ms\n");
        //    SDL_Delay(10);
        //    continue;
        //}
        if(av_read_frame(dec->fmtctx, &packet) < 0) {
            if(dec->fmtctx->pb->error == 0) {
                SDL_Delay(100); /* no error; wait for user input */
                continue;
            } else {
                break;
            }
        }
        // dec thdec a packet from the video stream?
        //av_log(dec, AV_LOG_INFO, "stream index=%d(video=%d audio=%d)\n", packet.stream_index, dec->video_stream->index, dec->audio_stream->index);
        if(packet.stream_index == dec->video_stream->index) {
            //av_log(dec, AV_LOG_INFO, "video packet(%d) pts=%ld\n", packet.stream_index, packet.pts);
            packet_queue_put(&dec->videoq, &packet);
        } else if(packet.stream_index == dec->audio_stream->index) {
            //av_log(dec, AV_LOG_INFO, "audio packet(%d) pts=%ld\n", packet.stream_index, packet.pts);
            packet_queue_put(&dec->audioq, &packet);
        } else {
            av_free_packet(&packet);
        }
    }
    /* all done - wait for it */
    while(!dec->quit) {
        SDL_Delay(100);
    }

    return 0;
}

int
ffva_decoder_parse_thread(FFVADecoder *dec)
{
    pthread_t thr1;
    int ret;

    dec->parse_tid = SDL_CreateThread(parse_thread, dec);
    //ret = pthread_create(&thr1, NULL, parse_thread, dec);
    if(!dec->parse_tid) {
    //if(ret < 0) {
        av_log(dec, AV_LOG_ERROR, "%s:create thread failed!\n", __func__);
        return -1;
    }
    av_log(dec, AV_LOG_DEBUG, "%s\n", __func__);
    return 0;
}

int video_thread(void *arg)
{
    FFVADecoder *dec = arg;
    AVPacket pkt1, *packet = &pkt1;
    int got_frame, ret;
    char errbuf[BUFSIZ];
    double pts;

    for(;;) {
        //if (dec->videoq.nb_packets != 0)
        //    av_log(dec, AV_LOG_INFO, "get video packet number:%d\n", dec->videoq.nb_packets);

        if(packet_queue_get(&dec->videoq, packet, 1) < 0) {
            // means we quit getting packets
            av_log(dec, AV_LOG_ERROR, "%s:get packet error\n", __func__);
            break;
        }

        ret = avcodec_decode_video2(dec->video_avctx, dec->video_frame, &got_frame, packet);
        if (ret < 0) {
            av_log(dec, AV_LOG_ERROR, "failed to decode frame: %s\n", ffmpeg_strerror(ret, errbuf));
            continue;
        }

        //if((pts = av_frame_get_best_effort_timestamp(dec->video_frame)) == AV_NOPTS_VALUE) {
        //} else {
        //    pts = 0;
        //}
        //pts *= av_q2d(dec->video_stream->time_base);
        pts = packet->pts * av_q2d(dec->video_stream->time_base);

        // Did we get a video frame?
        if(got_frame) {
            pts = synchronize_video(dec, pts);
            ret = handle_frame(dec, dec->video_frame);
            if (ret != 0) {
                break;
            }
            if (queue_picture(dec, dec->decoded_frame, pts) < 0) {
                break;
            }
        }
        av_free_packet(packet);
    }

    return 0;
}

int
ffva_decoder_video_thread(FFVADecoder *dec)
{
    SDL_Event       event;
    int ret;

    av_log(dec, AV_LOG_DEBUG, "%s\n", __func__);

    dec->pictq_mutex = SDL_CreateMutex();
    dec->pictq_cond = SDL_CreateCond();

    schedule_refresh(dec, 40);

    dec->video_tid = SDL_CreateThread(video_thread, dec);
    if(!dec->video_tid) {
        av_log(dec, AV_LOG_ERROR, "%s:create thread failed!\n", __func__);
        return -1;
    }

    for(;;) {
        ret = SDL_WaitEvent(&event);
        //av_log(dec, AV_LOG_INFO, "%s:ret=%d event type=%d\n", __func__, ret, event.type);
        switch(event.type) {
            case SDL_QUIT:
                dec->quit = 1;
                SDL_Quit();
                break;
            case FF_REFRESH_EVENT:
                video_refresh_timer(event.user.data1);
                break;
            default:
                break;
        }
    }
    return 0;
}
