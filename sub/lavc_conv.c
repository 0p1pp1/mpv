/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <assert.h>

#include <libavcodec/avcodec.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/common.h>
#include <libavutil/opt.h>

#include "config.h"

#include "mpv_talloc.h"
#include "common/msg.h"
#include "common/av_common.h"
#include "misc/bstr.h"
#include "sd.h"

struct lavc_conv {
    struct mp_log *log;
    AVCodecContext *avctx;
    char *codec;
    char *extradata;
    AVSubtitle cur;
    char **cur_list;
};

static const char *get_lavc_format(const char *format)
{
    // For the hack involving parse_webvtt().
    if (format && strcmp(format, "webvtt-webm") == 0)
        format = "webvtt";
    // Most text subtitles are srt/html style anyway.
    if (format && strcmp(format, "text") == 0)
        format = "subrip";
    return format;
}

// Disable style definitions generated by the libavcodec converter.
// We always want the user defined style instead.
static void disable_styles(bstr header)
{
    bstr style = bstr0("\nStyle: ");
    while (header.len) {
        int n = bstr_find(header, style);
        if (n < 0)
            break;
        header.start[n + 1] = '#'; // turn into a comment
        header = bstr_cut(header, n + style.len);
    }
}

struct lavc_conv *lavc_conv_create(struct mp_log *log, const char *codec_name,
                                   char *extradata, int extradata_len)
{
    struct lavc_conv *priv = talloc_zero(NULL, struct lavc_conv);
    priv->log = log;
    priv->cur_list = talloc_array(priv, char*, 0);
    priv->codec = talloc_strdup(priv, codec_name);
    AVCodecContext *avctx = NULL;
    AVDictionary *opts = NULL;
    const char *fmt = get_lavc_format(priv->codec);
    AVCodec *codec = avcodec_find_decoder(mp_codec_to_av_codec_id(fmt));
    if (!codec)
        goto error;
    avctx = avcodec_alloc_context3(codec);
    if (!avctx)
        goto error;
    if (mp_lavc_set_extradata(avctx, extradata, extradata_len) < 0)
        goto error;
    av_dict_set(&opts, "sub_text_format", "ass", 0);
    av_dict_set(&opts, "flags2", "+ass_ro_flush_noop", 0);
    if (strcmp(codec_name, "eia_608") == 0)
        av_dict_set(&opts, "real_time", "1", 0);
    if (avcodec_open2(avctx, codec, &opts) < 0)
        goto error;
    av_dict_free(&opts);
    // Documented as "set by libavcodec", but there is no other way
    avctx->time_base = (AVRational) {1, 1000};
    avctx->pkt_timebase = avctx->time_base;
    avctx->sub_charenc_mode = FF_SUB_CHARENC_MODE_IGNORE;
    priv->avctx = avctx;
    priv->extradata = talloc_strndup(priv, avctx->subtitle_header,
                                     avctx->subtitle_header_size);
    disable_styles(bstr0(priv->extradata));
    return priv;

 error:
    MP_FATAL(priv, "Could not open libavcodec subtitle converter\n");
    av_dict_free(&opts);
    av_free(avctx);
    talloc_free(priv);
    return NULL;
}

char *lavc_conv_get_extradata(struct lavc_conv *priv)
{
    return priv->extradata;
}

int lavc_conv_setopt_int(struct lavc_conv *priv, const char *name, int val)
{
    return av_opt_set_int(priv->avctx, name, val, AV_OPT_SEARCH_CHILDREN);
}

// FFmpeg WebVTT packets are pre-parsed in some way. The FFmpeg Matroska
// demuxer does this on its own. In order to free our demuxer_mkv.c from
// codec-specific crud, we do this here.
// Copied from libavformat/matroskadec.c (FFmpeg 818ebe9 / 2013-08-19)
// License: LGPL v2.1 or later
// Author header: The FFmpeg Project
// Modified in some ways.
static int parse_webvtt(AVPacket *in, AVPacket *pkt)
{
    uint8_t *id, *settings, *text, *buf;
    int id_len, settings_len, text_len;
    uint8_t *p, *q;
    int err;

    uint8_t *data = in->data;
    int data_len = in->size;

    if (data_len <= 0)
        return AVERROR_INVALIDDATA;

    p = data;
    q = data + data_len;

    id = p;
    id_len = -1;
    while (p < q) {
        if (*p == '\r' || *p == '\n') {
            id_len = p - id;
            if (*p == '\r')
                p++;
            break;
        }
        p++;
    }

    if (p >= q || *p != '\n')
        return AVERROR_INVALIDDATA;
    p++;

    settings = p;
    settings_len = -1;
    while (p < q) {
        if (*p == '\r' || *p == '\n') {
            settings_len = p - settings;
            if (*p == '\r')
                p++;
            break;
        }
        p++;
    }

    if (p >= q || *p != '\n')
        return AVERROR_INVALIDDATA;
    p++;

    text = p;
    text_len = q - p;
    while (text_len > 0) {
        const int len = text_len - 1;
        const uint8_t c = p[len];
        if (c != '\r' && c != '\n')
            break;
        text_len = len;
    }

    if (text_len <= 0)
        return AVERROR_INVALIDDATA;

    err = av_new_packet(pkt, text_len);
    if (err < 0)
        return AVERROR(err);

    memcpy(pkt->data, text, text_len);

    if (id_len > 0) {
        buf = av_packet_new_side_data(pkt,
                                      AV_PKT_DATA_WEBVTT_IDENTIFIER,
                                      id_len);
        if (buf == NULL) {
            av_packet_unref(pkt);
            return AVERROR(ENOMEM);
        }
        memcpy(buf, id, id_len);
    }

    if (settings_len > 0) {
        buf = av_packet_new_side_data(pkt,
                                      AV_PKT_DATA_WEBVTT_SETTINGS,
                                      settings_len);
        if (buf == NULL) {
            av_packet_unref(pkt);
            return AVERROR(ENOMEM);
        }
        memcpy(buf, settings, settings_len);
    }

    pkt->pts = in->pts;
    pkt->duration = in->duration;
    return 0;
}

// Return a NULL-terminated list of ASS event lines and have
// the AVSubtitle display PTS and duration set to input
// double variables.
char **lavc_conv_decode(struct lavc_conv *priv, struct demux_packet *packet,
                        double *sub_pts, double *sub_duration)
{
    AVCodecContext *avctx = priv->avctx;
    AVPacket pkt;
    AVPacket parsed_pkt = {0};
    int ret, got_sub;
    int num_cur = 0;

    avsubtitle_free(&priv->cur);

    mp_set_av_packet(&pkt, packet, &avctx->time_base);
    if (pkt.pts < 0)
        pkt.pts = 0;

    if (strcmp(priv->codec, "webvtt-webm") == 0) {
        if (parse_webvtt(&pkt, &parsed_pkt) < 0) {
            MP_ERR(priv, "Error parsing subtitle\n");
            goto done;
        }
        pkt = parsed_pkt;
    }

    ret = avcodec_decode_subtitle2(avctx, &priv->cur, &got_sub, &pkt);
    if (ret < 0) {
        MP_ERR(priv, "Error decoding subtitle\n");
    } else if (got_sub) {
        *sub_pts = packet->pts + mp_pts_from_av(priv->cur.start_display_time,
                                               &avctx->time_base);
        *sub_duration = priv->cur.end_display_time == UINT32_MAX ?
                        UINT32_MAX :
                        mp_pts_from_av(priv->cur.end_display_time -
                                       priv->cur.start_display_time,
                                       &avctx->time_base);

        for (int i = 0; i < priv->cur.num_rects; i++) {
            if (priv->cur.rects[i]->w > 0 && priv->cur.rects[i]->h > 0)
                MP_WARN(priv, "Ignoring bitmap subtitle.\n");
            char *ass_line = priv->cur.rects[i]->ass;
            if (!ass_line)
                continue;
            MP_TARRAY_APPEND(priv, priv->cur_list, num_cur, ass_line);
        }
    }

done:
    av_packet_unref(&parsed_pkt);
    MP_TARRAY_APPEND(priv, priv->cur_list, num_cur, NULL);
    return priv->cur_list;
}

void lavc_conv_reset(struct lavc_conv *priv)
{
    avcodec_flush_buffers(priv->avctx);
}

void lavc_conv_uninit(struct lavc_conv *priv)
{
    avsubtitle_free(&priv->cur);
    avcodec_free_context(&priv->avctx);
    talloc_free(priv);
}
