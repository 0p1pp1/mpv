/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <stdbool.h>
#include <strings.h>
#include <inttypes.h>
#include <assert.h>

#include <libavutil/avutil.h>

#include "config.h"
#include "mpv_talloc.h"

#include "osdep/io.h"
#include "osdep/terminal.h"
#include "osdep/timer.h"

#include "common/msg.h"
#include "common/global.h"
#include "options/path.h"
#include "options/m_config.h"
#include "options/parse_configfile.h"
#include "common/playlist.h"
#include "options/options.h"
#include "options/m_property.h"
#include "common/common.h"
#include "common/encode.h"
#include "common/recorder.h"
#include "input/input.h"

#include "audio/audio.h"
#include "audio/audio_buffer.h"
#include "audio/decode/dec_audio.h"
#include "audio/out/ao.h"
#include "demux/demux.h"
#include "stream/stream.h"
#include "sub/dec_sub.h"
#include "external_files.h"
#include "video/decode/dec_video.h"
#include "video/out/vo.h"

#include "core.h"
#include "command.h"
#include "libmpv/client.h"

// Called by foreign threads when playback should be stopped and such.
void mp_abort_playback_async(struct MPContext *mpctx)
{
    mp_cancel_trigger(mpctx->playback_abort);

    pthread_mutex_lock(&mpctx->lock);
    if (mpctx->demuxer_cancel)
        mp_cancel_trigger(mpctx->demuxer_cancel);
    pthread_mutex_unlock(&mpctx->lock);
}

static void uninit_demuxer(struct MPContext *mpctx)
{
    for (int r = 0; r < NUM_PTRACKS; r++) {
        for (int t = 0; t < STREAM_TYPE_COUNT; t++)
            mpctx->current_track[r][t] = NULL;
    }
    talloc_free(mpctx->chapters);
    mpctx->chapters = NULL;
    mpctx->num_chapters = 0;

    // close demuxers for external tracks
    for (int n = mpctx->num_tracks - 1; n >= 0; n--) {
        mpctx->tracks[n]->selected = false;
        mp_remove_track(mpctx, mpctx->tracks[n]);
    }
    for (int i = 0; i < mpctx->num_tracks; i++) {
        sub_destroy(mpctx->tracks[i]->d_sub);
        talloc_free(mpctx->tracks[i]);
    }
    mpctx->num_tracks = 0;

    free_demuxer_and_stream(mpctx->demuxer);
    mpctx->demuxer = NULL;

    pthread_mutex_lock(&mpctx->lock);
    talloc_free(mpctx->demuxer_cancel);
    mpctx->demuxer_cancel = NULL;
    pthread_mutex_unlock(&mpctx->lock);
}

#define APPEND(s, ...) mp_snprintf_cat(s, sizeof(s), __VA_ARGS__)

static void print_stream(struct MPContext *mpctx, struct track *t)
{
    struct sh_stream *s = t->stream;
    const char *tname = "?";
    const char *selopt = "?";
    const char *langopt = "?";
    switch (t->type) {
    case STREAM_VIDEO:
        tname = "Video"; selopt = "vid"; langopt = NULL;
        break;
    case STREAM_AUDIO:
        tname = "Audio"; selopt = "aid"; langopt = "alang";
        break;
    case STREAM_SUB:
        tname = "Subs"; selopt = "sid"; langopt = "slang";
        break;
    }
    char b[2048] = {0};
    APPEND(b, " %3s %-5s", t->selected ? "(+)" : "", tname);
    APPEND(b, " --%s=%d", selopt, t->user_tid);
    if (t->type == STREAM_AUDIO && s && s->is_dmono) {
        APPEND(b, " --%s=", langopt);
        if (s->dmono_mode != DMONO_SUB)
            APPEND(b, "[");
        APPEND(b, "%s", t->lang);
        if (s->dmono_mode == DMONO_MAIN)
            APPEND(b, "]");
        APPEND(b, "/");
        if (s->dmono_mode == DMONO_SUB)
            APPEND(b, "[");
        APPEND(b, "%s", s->lang_sub ? s->lang_sub : t->lang);
        if (s->dmono_mode != DMONO_MAIN)
            APPEND(b, "]");
    } else
    if (t->lang && langopt)
        APPEND(b, " --%s=%s", langopt, t->lang);
    if (t->default_track)
        APPEND(b, " (*)");
    if (t->forced_track)
        APPEND(b, " (f)");
    if (t->attached_picture)
        APPEND(b, " [P]");
    if (t->title)
        APPEND(b, " '%s'", t->title);
    const char *codec = s ? s->codec->codec : NULL;
    APPEND(b, " (%s", codec ? codec : "<unknown>");
    if (t->type == STREAM_VIDEO) {
        if (s && s->codec->disp_w)
            APPEND(b, " %dx%d", s->codec->disp_w, s->codec->disp_h);
        if (s && s->codec->fps)
            APPEND(b, " %.3ffps", s->codec->fps);
    } else if (t->type == STREAM_AUDIO) {
        if (s && s->codec->channels.num)
            APPEND(b, " %dch", s->codec->channels.num);
        if (s && s->codec->samplerate)
            APPEND(b, " %dHz", s->codec->samplerate);
    }
    APPEND(b, ")");
    if (t->is_external)
        APPEND(b, " (external)");
    MP_INFO(mpctx, "%s\n", b);
}

void print_track_list(struct MPContext *mpctx, const char *msg)
{
    if (msg)
        MP_INFO(mpctx, "%s\n", msg);
    for (int t = 0; t < STREAM_TYPE_COUNT; t++) {
        for (int n = 0; n < mpctx->num_tracks; n++)
            if (mpctx->tracks[n]->type == t)
                print_stream(mpctx, mpctx->tracks[n]);
    }
}

static struct track *
track_by_dmxid(struct MPContext *mpctx, enum stream_type type, int dmxid)
{
    if (dmxid < 0)
        return NULL;

    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *track = mpctx->tracks[n];
        if (track->type == type && track->demuxer_id == dmxid)
            return track;
    }
    return NULL;
}

// try to recover from lost streams in mid stream (due to PMT change in TS).
// prepare for the future gapless switch of tracks.
static void recover_lost_streams(struct MPContext *mpctx)
{
    demux_program_t prog;

    prog.progid = PROGID_KEEP_CURRENT;
    prog.vlangs = mpctx->opts->stream_lang[STREAM_VIDEO];
    prog.alangs = mpctx->opts->stream_lang[STREAM_AUDIO];
    prog.slangs = mpctx->opts->stream_lang[STREAM_SUB];
    if (demux_control(mpctx->demuxer, DEMUXER_CTRL_IDENTIFY_PROGRAM, &prog)
        != DEMUXER_CTRL_OK)
        return;    // there's nothing we can do.

    MP_VERBOSE(mpctx, "setup switching to prog:%d v:%#06x a:%#06x s:%#06x\n",
               prog.progid, prog.vid, prog.aid, prog.sid);
    mpctx->next_track[STREAM_VIDEO] = track_by_dmxid(mpctx, STREAM_VIDEO, prog.vid);
    mpctx->next_track[STREAM_AUDIO] = track_by_dmxid(mpctx, STREAM_AUDIO, prog.aid);
    mpctx->next_track[STREAM_SUB] = track_by_dmxid(mpctx, STREAM_SUB, prog.sid);
    // activate the new demuxer stream here to preload packets,
    //  and stop feeding to the previous stream (but keep its packet buffer).
    // complete track-switch should be done later at handle_track_pivot().
    for (int type = 0; type < STREAM_TYPE_COUNT; type++) {
        struct track *t = mpctx->next_track[type];
        struct demux_packet *dummy;

        if (!t)
            continue;
        t->selected = true;
        // Selects new demuxer stream. This trigers refreshing seek,
        // which is desirable as some packets might have been discarded by now.
        reselect_demux_stream(mpctx, t);
        // make its demuxer_stream active
        demux_read_packet_async(t->stream, &dummy);

        // find the previous stream and stop it.
        for (int order = 0; order < NUM_PTRACKS; order++) {
            struct track *cur;

            // pivot only the streams from the same demuxer,
            // excluding the user specified pids.
            // this must be consistend with playloop.c::handle_track_pivot().
            if (mpctx->opts->stream_id[order][type] != -1)
                continue;

            cur = mpctx->current_track[order][type];
            if (!cur || cur->demuxer != t->demuxer)
                continue;

            MP_VERBOSE(cur->demuxer, "de-selecting [%04x]\n", cur->demuxer_id);
            cur->selected = false;
            demux_deactivate_stream(cur->demuxer, cur->stream);
            break;
        }
    }
}

void update_demuxer_properties(struct MPContext *mpctx)
{
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer)
        return;
    demux_update(demuxer);
    int events = demuxer->events;
    if ((events & DEMUX_EVENT_INIT) && demuxer->num_editions > 1) {
        for (int n = 0; n < demuxer->num_editions; n++) {
            struct demux_edition *edition = &demuxer->editions[n];
            char b[128] = {0};
            APPEND(b, " %3s --edition=%d",
                   n == demuxer->edition ? "(+)" : "", n);
            char *name = mp_tags_get_str(edition->metadata, "title");
            if (name)
                APPEND(b, " '%s'", name);
            if (edition->default_edition)
                APPEND(b, " (*)");
            MP_INFO(mpctx, "%s\n", b);
        }
    }
    struct demuxer *tracks = mpctx->demuxer;
    if (tracks->events & DEMUX_EVENT_STREAMS) {
        add_demuxer_tracks(mpctx, tracks);
        print_track_list(mpctx, NULL);
        tracks->events &= ~DEMUX_EVENT_STREAMS;
    }
    if (mpctx->in_playloop && (events & DEMUX_EVENT_NOSTREAM) &&
        !(mpctx->next_track[0] || mpctx->next_track[1] || mpctx->next_track[2])) {
        MP_INFO(mpctx, "current track was lost.\n");
        recover_lost_streams(mpctx);
    }
    if (events & DEMUX_EVENT_METADATA) {
        struct mp_tags *info =
            mp_tags_filtered(mpctx, demuxer->metadata, mpctx->opts->display_tags);
        // prev is used to attempt to print changed tags only (to some degree)
        struct mp_tags *prev = mpctx->filtered_tags;
        int n_prev = 0;
        bool had_output = false;
        for (int n = 0; n < info->num_keys; n++) {
            if (prev && n_prev < prev->num_keys) {
                if (strcmp(prev->keys[n_prev], info->keys[n]) == 0) {
                    n_prev++;
                    if (strcmp(prev->values[n_prev - 1], info->values[n]) == 0)
                        continue;
                }
            }
            struct mp_log *log = mp_log_new(NULL, mpctx->log, "!display-tags");
            if (!had_output)
                mp_info(log, "File tags:\n");
            mp_info(log, " %s: %s\n", info->keys[n], info->values[n]);
            had_output = true;
            talloc_free(log);
        }
        talloc_free(mpctx->filtered_tags);
        mpctx->filtered_tags = info;
        mp_notify(mpctx, MPV_EVENT_METADATA_UPDATE, NULL);
    }
    demuxer->events = 0;
}

// Enables or disables the stream for the given track, according to
// track->selected.
void reselect_demux_stream(struct MPContext *mpctx, struct track *track)
{
    if (!track->stream)
        return;
    double pts = get_current_time(mpctx);
    if (pts != MP_NOPTS_VALUE)
        pts += get_track_seek_offset(mpctx, track);
    demuxer_select_track(track->demuxer, track->stream, pts, track->selected);
}

// Called from the demuxer thread if a new packet is available.
static void wakeup_demux(void *pctx)
{
    struct MPContext *mpctx = pctx;
    mp_wakeup_core(mpctx);
}

static void enable_demux_thread(struct MPContext *mpctx, struct demuxer *demux)
{
    if (mpctx->opts->demuxer_thread && !demux->fully_read) {
        demux_set_wakeup_cb(demux, wakeup_demux, mpctx);
        demux_start_thread(demux);
    }
}

static int find_new_tid(struct MPContext *mpctx, enum stream_type t)
{
    int new_id = 0;
    for (int i = 0; i < mpctx->num_tracks; i++) {
        struct track *track = mpctx->tracks[i];
        if (track->type == t)
            new_id = MPMAX(new_id, track->user_tid);
    }
    return new_id + 1;
}

static struct track *add_stream_track(struct MPContext *mpctx,
                                      struct demuxer *demuxer,
                                      struct sh_stream *stream)
{
    for (int i = 0; i < mpctx->num_tracks; i++) {
        struct track *track = mpctx->tracks[i];
        if (track->stream == stream)
            return track;
    }

    struct track *track = talloc_ptrtype(NULL, track);
    *track = (struct track) {
        .type = stream->type,
        .user_tid = find_new_tid(mpctx, stream->type),
        .demuxer_id = stream->demuxer_id,
        .ff_index = stream->ff_index,
        .title = stream->title,
        .default_track = stream->default_track,
        .forced_track = stream->forced_track,
        .attached_picture = stream->attached_picture != NULL,
        .lang = stream->lang,
        .demuxer = demuxer,
        .stream = stream,
    };
    MP_TARRAY_APPEND(mpctx, mpctx->tracks, mpctx->num_tracks, track);

    demuxer_select_track(track->demuxer, stream, MP_NOPTS_VALUE, false);

    mp_notify(mpctx, MPV_EVENT_TRACKS_CHANGED, NULL);

    return track;
}

void add_demuxer_tracks(struct MPContext *mpctx, struct demuxer *demuxer)
{
    for (int n = 0; n < demux_get_num_stream(demuxer); n++)
        add_stream_track(mpctx, demuxer, demux_get_stream(demuxer, n));
}

// Result numerically higher => better match. 0 == no match.
static int match_lang(char **langs, char *lang)
{
    for (int idx = 0; langs && langs[idx]; idx++) {
        if (lang && strcmp(langs[idx], lang) == 0)
            return INT_MAX - idx;
    }
    return 0;
}

/* Get the track wanted by the user.
 * tid is the track ID requested by the user (-2: deselect, -1: default)
 * lang is a string list, NULL is same as empty list
 * Sort tracks based on the following criteria, and pick the first:
 * 0a) track matches ff-index (always wins)
 * 0b) track matches tid (almost always wins)
 * 1) track is external (no_default cancels this)
 * 1b) track was passed explicitly (is not an auto-loaded subtitle)
 * 2) earlier match in lang list
 * 3a) track is marked forced
 * 3b) track is marked default
 * 4) attached picture, HLS bitrate
 * 5) lower track number
 * If select_fallback is not set, 5) is only used to determine whether a
 * matching track is preferred over another track. Otherwise, always pick a
 * track (if nothing else matches, return the track with lowest ID).
 */
// Return whether t1 is preferred over t2
static bool compare_track(struct track *t1, struct track *t2, char **langs,
                          struct MPOpts *opts)
{
    if (!opts->autoload_files && t1->is_external != t2->is_external)
        return !t1->is_external;
    bool ext1 = t1->is_external && !t1->no_default;
    bool ext2 = t2->is_external && !t2->no_default;
    if (ext1 != ext2)
        return ext1;
    if (t1->auto_loaded != t2->auto_loaded)
        return !t1->auto_loaded;
    int l1 = match_lang(langs, t1->lang), l2 = match_lang(langs, t2->lang);
    int l1s = (t1->stream && t1->stream->is_dmono) ?
                    match_lang(langs, t1->stream->lang_sub) : 0;
    int l2s = (t2->stream && t2->stream->is_dmono) ?
                    match_lang(langs, t2->stream->lang_sub) : 0;
    if (l1s > l1) l1 = l1s;
    if (l2s > l2) l2 = l2s;
    if (l1 != l2)
        return l1 > l2;
    if (t1->forced_track != t2->forced_track)
        return t1->forced_track;
    if (t1->default_track != t2->default_track)
        return t1->default_track;
    if (t1->attached_picture != t2->attached_picture)
        return !t1->attached_picture;
    if (t1->stream && t2->stream && opts->hls_bitrate >= 0 &&
        t1->stream->hls_bitrate != t2->stream->hls_bitrate)
    {
        bool t1_ok = t1->stream->hls_bitrate <= opts->hls_bitrate;
        bool t2_ok = t2->stream->hls_bitrate <= opts->hls_bitrate;
        if (t1_ok != t2_ok)
            return t1_ok;
        if (t1_ok && t2_ok)
            return t1->stream->hls_bitrate > t2->stream->hls_bitrate;
        return t1->stream->hls_bitrate < t2->stream->hls_bitrate;
    }
    return t1->user_tid <= t2->user_tid;
}
struct track *select_default_track(struct MPContext *mpctx, int order,
                                   enum stream_type type)
{
    struct MPOpts *opts = mpctx->opts;
    int tid = opts->stream_id[order][type];
    int ffid = order == 0 ? opts->stream_id_ff[type] : -1;
    char **langs = order == 0 ? opts->stream_lang[type] : NULL;
    if (ffid != -1)
        tid = -1; // prefer selecting ffid
    if (tid == -2 || ffid == -2)
        return NULL;
    bool select_fallback = type == STREAM_VIDEO || type == STREAM_AUDIO;
    struct track *pick = NULL;
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *track = mpctx->tracks[n];
        if (track->type != type)
            continue;
        if (track->user_tid == tid)
            return track;
        if (track->ff_index == ffid)
            return track;
        if (!pick || compare_track(track, pick, langs, mpctx->opts))
            pick = track;
    }
    if (pick && !select_fallback && !(pick->is_external && !pick->no_default)
        && !match_lang(langs, pick->lang) && !pick->default_track
        && !pick->forced_track)
        pick = NULL;
    if (pick && pick->attached_picture && !mpctx->opts->audio_display)
        pick = NULL;
    if (pick && !opts->autoload_files && pick->is_external)
        pick = NULL;
    return pick;
}

static char *track_layout_hash(struct MPContext *mpctx)
{
    char *h = talloc_strdup(NULL, "");
    for (int type = 0; type < STREAM_TYPE_COUNT; type++) {
        for (int n = 0; n < mpctx->num_tracks; n++) {
            struct track *track = mpctx->tracks[n];
            if (track->type != type)
                continue;
            h = talloc_asprintf_append_buffer(h, "%d-%d-%d-%d-%s\n", type,
                    track->user_tid, track->default_track, track->is_external,
                    track->lang ? track->lang : "");
        }
    }
    return h;
}

// Normally, video/audio/sub track selection is persistent across files. This
// code resets track selection if the new file has a different track layout.
static void check_previous_track_selection(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;

    if (!mpctx->track_layout_hash)
        return;

    char *h = track_layout_hash(mpctx);
    if (strcmp(h, mpctx->track_layout_hash) != 0) {
        // Reset selection, but only if they're not "auto" or "off". The
        // defaults are -1 (default selection), or -2 (off) for secondary tracks.
        for (int t = 0; t < STREAM_TYPE_COUNT; t++) {
            for (int i = 0; i < NUM_PTRACKS; i++) {
                if (opts->stream_id[i][t] >= 0)
                    opts->stream_id[i][t] = i == 0 ? -1 : -2;
            }
        }
        talloc_free(mpctx->track_layout_hash);
        mpctx->track_layout_hash = NULL;
    }
    talloc_free(h);
}

void mp_switch_track_n(struct MPContext *mpctx, int order, enum stream_type type,
                       struct track *track, int flags)
{
    assert(!track || track->type == type);
    assert(order >= 0 && order < NUM_PTRACKS);

    // Mark the current track selection as explicitly user-requested. (This is
    // different from auto-selection or disabling a track due to errors.)
    if (flags & FLAG_MARK_SELECTION)
        mpctx->opts->stream_id[order][type] = track ? track->user_tid : -2;

    // No decoder should be initialized yet.
    if (!mpctx->demuxer)
        return;

    struct track *current = mpctx->current_track[order][type];
    if (track == current)
        return;

    if (current && current->sink) {
        MP_ERR(mpctx, "Can't disable input to complex filter.\n");
        return;
    }
    if ((type == STREAM_VIDEO && mpctx->vo_chain && !mpctx->vo_chain->track) ||
        (type == STREAM_AUDIO && mpctx->ao_chain && !mpctx->ao_chain->track))
    {
        MP_ERR(mpctx, "Can't switch away from complex filter output.\n");
        return;
    }

    if (track && track->selected) {
        // Track has been selected in a different order parameter.
        MP_ERR(mpctx, "Track %d is already selected.\n", track->user_tid);
        return;
    }

    if (order == 0) {
        if (type == STREAM_VIDEO) {
            uninit_video_chain(mpctx);
            if (!track)
                handle_force_window(mpctx, true);
        } else if (type == STREAM_AUDIO) {
            clear_audio_output_buffers(mpctx);
            uninit_audio_chain(mpctx);
            uninit_audio_out(mpctx);
        }
    }
    if (type == STREAM_SUB)
        uninit_sub(mpctx, current);

    if (current) {
        if (current->remux_sink)
            close_recorder_and_error(mpctx);
        current->selected = false;
        reselect_demux_stream(mpctx, current);
    }

    mpctx->current_track[order][type] = track;

    if (track) {
        track->selected = true;
        reselect_demux_stream(mpctx, track);
    }

    if (type == STREAM_VIDEO && order == 0) {
        reinit_video_chain(mpctx);
    } else if (type == STREAM_AUDIO && order == 0) {
        reinit_audio_chain(mpctx);
    } else if (type == STREAM_SUB && order >= 0 && order <= 2) {
        reinit_sub(mpctx, track);
    }

    mp_notify(mpctx, MPV_EVENT_TRACK_SWITCHED, NULL);
    mp_wakeup_core(mpctx);

    talloc_free(mpctx->track_layout_hash);
    mpctx->track_layout_hash = talloc_steal(mpctx, track_layout_hash(mpctx));
}

void mp_switch_track(struct MPContext *mpctx, enum stream_type type,
                     struct track *track, int flags)
{
    mp_switch_track_n(mpctx, 0, type, track, flags);
}

void mp_deselect_track(struct MPContext *mpctx, struct track *track)
{
    if (track && track->selected) {
        for (int t = 0; t < NUM_PTRACKS; t++)
            mp_switch_track_n(mpctx, t, track->type, NULL, 0);
    }
}

struct track *mp_track_by_tid(struct MPContext *mpctx, enum stream_type type,
                              int tid)
{
    if (tid == -1)
        return mpctx->current_track[0][type];
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *track = mpctx->tracks[n];
        if (track->type == type && track->user_tid == tid)
            return track;
    }
    return NULL;
}

bool mp_remove_track(struct MPContext *mpctx, struct track *track)
{
    if (!track->is_external)
        return false;

    mp_deselect_track(mpctx, track);
    if (track->selected)
        return false;

    struct demuxer *d = track->demuxer;

    sub_destroy(track->d_sub);

    int index = 0;
    while (index < mpctx->num_tracks && mpctx->tracks[index] != track)
        index++;
    MP_TARRAY_REMOVE_AT(mpctx->tracks, mpctx->num_tracks, index);
    talloc_free(track);

    // Close the demuxer, unless there is still a track using it. These are
    // all external tracks.
    bool in_use = false;
    for (int n = mpctx->num_tracks - 1; n >= 0 && !in_use; n--)
        in_use |= mpctx->tracks[n]->demuxer == d;

    if (!in_use)
        free_demuxer_and_stream(d);

    mp_notify(mpctx, MPV_EVENT_TRACKS_CHANGED, NULL);

    return true;
}

// Add the given file as additional track. Only tracks of type "filter" are
// included; pass STREAM_TYPE_COUNT to disable filtering.
struct track *mp_add_external_file(struct MPContext *mpctx, char *filename,
                                   enum stream_type filter)
{
    struct MPOpts *opts = mpctx->opts;
    if (!filename)
        return NULL;

    char *disp_filename = filename;
    if (strncmp(disp_filename, "memory://", 9) == 0)
        disp_filename = "memory://"; // avoid noise

    struct demuxer_params params = {0};

    switch (filter) {
    case STREAM_SUB:
        params.force_format = opts->sub_demuxer_name;
        break;
    case STREAM_AUDIO:
        params.force_format = opts->audio_demuxer_name;
        break;
    }

    struct demuxer *demuxer =
        demux_open_url(filename, &params, mpctx->playback_abort, mpctx->global);
    if (!demuxer)
        goto err_out;
    enable_demux_thread(mpctx, demuxer);

    if (filter != STREAM_SUB && opts->rebase_start_time)
        demux_set_ts_offset(demuxer, -demuxer->start_time);

    struct track *first = NULL;
    for (int n = 0; n < demux_get_num_stream(demuxer); n++) {
        struct sh_stream *sh = demux_get_stream(demuxer, n);
        if (filter == STREAM_TYPE_COUNT || sh->type == filter) {
            struct track *t = add_stream_track(mpctx, demuxer, sh);
            t->is_external = true;
            t->title = talloc_strdup(t, mp_basename(disp_filename));
            t->external_filename = talloc_strdup(t, filename);
            first = t;
            // --external-file special semantics
            t->no_default = filter == STREAM_TYPE_COUNT;
        }
    }
    if (!first) {
        free_demuxer_and_stream(demuxer);
        MP_WARN(mpctx, "No streams added from file %s.\n", disp_filename);
        goto err_out;
    }

    return first;

err_out:
    MP_ERR(mpctx, "Can not open external file %s.\n", disp_filename);
    return false;
}

static void open_external_files(struct MPContext *mpctx, char **files,
                                enum stream_type filter)
{
    for (int n = 0; files && files[n]; n++)
        mp_add_external_file(mpctx, files[n], filter);
}

void autoload_external_files(struct MPContext *mpctx)
{
    if (mpctx->opts->sub_auto < 0 && mpctx->opts->audiofile_auto < 0)
        return;
    if (!mpctx->opts->autoload_files)
        return;

    void *tmp = talloc_new(NULL);
    char *base_filename = mpctx->filename;
    char *stream_filename = NULL;
    if (mpctx->demuxer) {
        if (demux_stream_control(mpctx->demuxer, STREAM_CTRL_GET_BASE_FILENAME,
                                    &stream_filename) > 0)
            base_filename = talloc_steal(tmp, stream_filename);
    }
    struct subfn *list = find_external_files(mpctx->global, base_filename);
    talloc_steal(tmp, list);

    int sc[STREAM_TYPE_COUNT] = {0};
    for (int n = 0; n < mpctx->num_tracks; n++) {
        if (!mpctx->tracks[n]->attached_picture)
            sc[mpctx->tracks[n]->type]++;
    }

    for (int i = 0; list && list[i].fname; i++) {
        char *filename = list[i].fname;
        char *lang = list[i].lang;
        for (int n = 0; n < mpctx->num_tracks; n++) {
            struct track *t = mpctx->tracks[n];
            if (t->demuxer && strcmp(t->demuxer->filename, filename) == 0)
                goto skip;
        }
        if (list[i].type == STREAM_SUB && !sc[STREAM_VIDEO] && !sc[STREAM_AUDIO])
            goto skip;
        if (list[i].type == STREAM_AUDIO && !sc[STREAM_VIDEO])
            goto skip;
        struct track *track = mp_add_external_file(mpctx, filename, list[i].type);
        if (track) {
            track->auto_loaded = true;
            if (!track->lang)
                track->lang = talloc_strdup(track, lang);
        }
    skip:;
    }

    talloc_free(tmp);
}

// Do stuff to a newly loaded playlist. This includes any processing that may
// be required after loading a playlist.
void prepare_playlist(struct MPContext *mpctx, struct playlist *pl)
{
    struct MPOpts *opts = mpctx->opts;

    pl->current = NULL;

    if (opts->playlist_pos >= 0)
        pl->current = playlist_entry_from_index(pl, opts->playlist_pos);

    if (opts->shuffle)
        playlist_shuffle(pl);

    if (opts->merge_files)
        merge_playlist_files(pl);

    if (!pl->current)
        pl->current = mp_check_playlist_resume(mpctx, pl);

    if (!pl->current)
        pl->current = pl->first;
}

// Replace the current playlist entry with playlist contents. Moves the entries
// from the given playlist pl, so the entries don't actually need to be copied.
static void transfer_playlist(struct MPContext *mpctx, struct playlist *pl)
{
    if (pl->first) {
        prepare_playlist(mpctx, pl);
        struct playlist_entry *new = pl->current;
        if (mpctx->playlist->current)
            playlist_add_redirect(pl, mpctx->playlist->current->filename);
        playlist_transfer_entries(mpctx->playlist, pl);
        // current entry is replaced
        if (mpctx->playlist->current)
            playlist_remove(mpctx->playlist, mpctx->playlist->current);
        if (new)
            mpctx->playlist->current = new;
    } else {
        MP_WARN(mpctx, "Empty playlist!\n");
    }
}

static int process_open_hooks(struct MPContext *mpctx)
{

    mp_hook_run(mpctx, NULL, "on_load");

    while (!mp_hook_test_completion(mpctx, "on_load")) {
        mp_idle(mpctx);
        if (mpctx->stop_play) {
            // Can't exit immediately, the script would interfere with the
            // next file being loaded.
            if (mpctx->stop_play == PT_QUIT)
                return -1;
        }
    }

    return 0;
}

static int process_preloaded_hooks(struct MPContext *mpctx)
{
    mp_hook_run(mpctx, NULL, "on_preloaded");

    while (!mp_hook_test_completion(mpctx, "on_preloaded")) {
        mp_idle(mpctx);
        if (mpctx->stop_play)
            return -1;
    }

    return 0;
}

static void process_unload_hooks(struct MPContext *mpctx)
{
    mp_hook_run(mpctx, NULL, "on_unload");

    while (!mp_hook_test_completion(mpctx, "on_unload"))
        mp_idle(mpctx);
}

static void load_chapters(struct MPContext *mpctx)
{
    struct demuxer *src = mpctx->demuxer;
    bool free_src = false;
    char *chapter_file = mpctx->opts->chapter_file;
    if (chapter_file && chapter_file[0]) {
        struct demuxer *demux = demux_open_url(chapter_file, NULL,
                                        mpctx->playback_abort, mpctx->global);
        if (demux) {
            src = demux;
            free_src = true;
        }
        talloc_free(mpctx->chapters);
        mpctx->chapters = NULL;
    }
    if (src && !mpctx->chapters) {
        talloc_free(mpctx->chapters);
        mpctx->num_chapters = src->num_chapters;
        mpctx->chapters = demux_copy_chapter_data(src->chapters, src->num_chapters);
        if (mpctx->opts->rebase_start_time) {
            for (int n = 0; n < mpctx->num_chapters; n++)
                mpctx->chapters[n].pts -= src->start_time;
        }
    }
    if (free_src)
        free_demuxer_and_stream(src);
}

static void load_per_file_options(m_config_t *conf,
                                  struct playlist_param *params,
                                  int params_count)
{
    for (int n = 0; n < params_count; n++) {
        m_config_set_option_ext(conf, params[n].name, params[n].value,
                                M_SETOPT_RUNTIME | M_SETOPT_BACKUP);
    }
}

static void *open_demux_thread(void *ctx)
{
    struct MPContext *mpctx = ctx;

    struct demuxer_params p = {
        .force_format = mpctx->open_format,
        .stream_flags = mpctx->open_url_flags,
        .initial_readahead = true,
    };
    mpctx->open_res_demuxer =
        demux_open_url(mpctx->open_url, &p, mpctx->open_cancel, mpctx->global);

    if (mpctx->open_res_demuxer) {
        MP_VERBOSE(mpctx, "Opening done: %s\n", mpctx->open_url);
    } else {
        MP_VERBOSE(mpctx, "Opening failed or was aborted: %s\n", mpctx->open_url);

        if (p.demuxer_failed) {
            mpctx->open_res_error = MPV_ERROR_UNKNOWN_FORMAT;
        } else {
            mpctx->open_res_error = MPV_ERROR_LOADING_FAILED;
        }
    }

    atomic_store(&mpctx->open_done, true);
    mp_wakeup_core(mpctx);
    return NULL;
}

static void cancel_open(struct MPContext *mpctx)
{
    if (mpctx->open_cancel)
        mp_cancel_trigger(mpctx->open_cancel);

    if (mpctx->open_active)
        pthread_join(mpctx->open_thread, NULL);
    mpctx->open_active = false;

    TA_FREEP(&mpctx->open_cancel);
    TA_FREEP(&mpctx->open_url);
    TA_FREEP(&mpctx->open_format);

    if (mpctx->open_res_demuxer)
        free_demuxer_and_stream(mpctx->open_res_demuxer);
    mpctx->open_res_demuxer = NULL;

    atomic_store(&mpctx->open_done, false);
}

// Setup all the field to open this url, and make sure a thread is running.
static void start_open(struct MPContext *mpctx, char *url, int url_flags)
{
    cancel_open(mpctx);

    assert(!mpctx->open_active);
    assert(!mpctx->open_cancel);
    assert(!mpctx->open_res_demuxer);
    assert(!atomic_load(&mpctx->open_done));

    mpctx->open_cancel = mp_cancel_new(NULL);
    mpctx->open_url = talloc_strdup(NULL, url);
    mpctx->open_format = talloc_strdup(NULL, mpctx->opts->demuxer_name);
    mpctx->open_url_flags = url_flags;
    if (mpctx->opts->load_unsafe_playlists)
        mpctx->open_url_flags = 0;

    if (pthread_create(&mpctx->open_thread, NULL, open_demux_thread, mpctx)) {
        cancel_open(mpctx);
        return;
    }

    mpctx->open_active = true;
}

static void open_demux_reentrant(struct MPContext *mpctx)
{
    char *url = mpctx->stream_open_filename;

    if (mpctx->open_active) {
        bool done = atomic_load(&mpctx->open_done);
        bool failed = done && !mpctx->open_res_demuxer;
        bool correct_url = strcmp(mpctx->open_url, url) == 0;

        if (correct_url && !failed) {
            MP_VERBOSE(mpctx, "Using prefetched/prefetching URL.\n");
        } else if (correct_url && failed) {
            MP_VERBOSE(mpctx, "Prefetched URL failed, retrying.\n");
            cancel_open(mpctx);
        } else {
            if (done) {
                MP_VERBOSE(mpctx, "Dropping finished prefetch of wrong URL.\n");
            } else {
                MP_VERBOSE(mpctx, "Aborting onging prefetch of wrong URL.\n");
            }
            cancel_open(mpctx);
        }
    }

    if (!mpctx->open_active)
        start_open(mpctx, url, mpctx->playing->stream_flags);

    // User abort should cancel the opener now.
    pthread_mutex_lock(&mpctx->lock);
    mpctx->demuxer_cancel = mpctx->open_cancel;
    pthread_mutex_unlock(&mpctx->lock);

    while (!atomic_load(&mpctx->open_done)) {
        mp_idle(mpctx);

        if (mpctx->stop_play)
            mp_abort_playback_async(mpctx);
    }

    if (mpctx->open_res_demuxer) {
        assert(mpctx->demuxer_cancel == mpctx->open_cancel);
        mpctx->demuxer = mpctx->open_res_demuxer;
        mpctx->open_res_demuxer = NULL;
        mpctx->open_cancel = NULL;
    } else {
        mpctx->error_playing = mpctx->open_res_error;
        pthread_mutex_lock(&mpctx->lock);
        mpctx->demuxer_cancel = NULL;
        pthread_mutex_unlock(&mpctx->lock);
    }

    cancel_open(mpctx); // cleanup
}

void prefetch_next(struct MPContext *mpctx)
{
    if (!mpctx->opts->prefetch_open)
        return;

    struct playlist_entry *new_entry = mp_next_file(mpctx, +1, false, false);
    if (new_entry && !mpctx->open_active && new_entry->filename) {
        MP_VERBOSE(mpctx, "Prefetching: %s\n", new_entry->filename);
        start_open(mpctx, new_entry->filename, new_entry->stream_flags);
    }
}

static bool init_complex_filters(struct MPContext *mpctx)
{
    assert(!mpctx->lavfi);

    char *graph = mpctx->opts->lavfi_complex;

    if (!graph || !graph[0])
        return true;

    mpctx->lavfi = lavfi_create(mpctx->log, graph);
    if (!mpctx->lavfi)
        return false;

    if (lavfi_has_failed(mpctx->lavfi))
        return false;

    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *track = mpctx->tracks[n];

        char label[32];
        char prefix;
        switch (track->type) {
        case STREAM_VIDEO: prefix = 'v'; break;
        case STREAM_AUDIO: prefix = 'a'; break;
        default: continue;
        }
        snprintf(label, sizeof(label), "%cid%d", prefix, track->user_tid);

        struct lavfi_pad *pad = lavfi_find_pad(mpctx->lavfi, label);
        if (!pad)
            continue;
        if (lavfi_pad_type(pad) != track->type)
            continue;
        if (lavfi_pad_direction(pad) != LAVFI_IN)
            continue;
        if (lavfi_get_connected(pad))
            continue;

        track->sink = pad;
        lavfi_set_connected(pad, true);
        track->selected = true;
    }

    struct lavfi_pad *pad = lavfi_find_pad(mpctx->lavfi, "vo");
    if (pad && lavfi_pad_type(pad) == STREAM_VIDEO &&
        lavfi_pad_direction(pad) == LAVFI_OUT)
    {
        lavfi_set_connected(pad, true);
        reinit_video_chain_src(mpctx, pad);
    }

    pad = lavfi_find_pad(mpctx->lavfi, "ao");
    if (pad && lavfi_pad_type(pad) == STREAM_AUDIO &&
        lavfi_pad_direction(pad) == LAVFI_OUT)
    {
        lavfi_set_connected(pad, true);
        reinit_audio_chain_src(mpctx, pad);
    }

    return true;
}

static bool init_complex_filter_decoders(struct MPContext *mpctx)
{
    if (!mpctx->lavfi)
        return true;

    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *track = mpctx->tracks[n];
        if (track->sink && track->type == STREAM_VIDEO) {
            if (!init_video_decoder(mpctx, track))
                return false;
        }
        if (track->sink && track->type == STREAM_AUDIO) {
            if (!init_audio_decoder(mpctx, track))
                return false;
        }
    }

    return true;
}

static void uninit_complex_filters(struct MPContext *mpctx)
{
    if (!mpctx->lavfi)
        return;

    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *track = mpctx->tracks[n];

        if (track->d_video && !track->vo_c) {
            video_uninit(track->d_video);
            track->d_video = NULL;
        }
        if (track->d_audio && !track->ao_c) {
            audio_uninit(track->d_audio);
            track->d_audio = NULL;
        }
    }

    if (mpctx->vo_chain && mpctx->vo_chain->filter_src)
        uninit_video_chain(mpctx);
    if (mpctx->ao_chain && mpctx->ao_chain->filter_src)
        uninit_audio_chain(mpctx);

    lavfi_destroy(mpctx->lavfi);
    mpctx->lavfi = NULL;
}

bool mp_track_is_dmono(struct track *track)
{
    return (track && track->type == STREAM_AUDIO
            && track->stream && track->stream->is_dmono);
}

void mp_select_dmono_sub_ch(struct MPContext *mpctx, struct track *track)
{
    struct MPOpts *opts = mpctx->opts;
    char **langs;
    struct sh_stream *sh;

    if (!mp_track_is_dmono(track))
        return;

    langs = opts ? opts->stream_lang[STREAM_AUDIO] : NULL;
    sh = track->stream;

    if (opts && opts->audio_dmono_ch != -1)
        sh->dmono_mode = opts->audio_dmono_ch;
    else if (match_lang(langs, sh->lang) < match_lang(langs, sh->lang_sub))
        sh->dmono_mode = DMONO_SUB;
    else
        sh->dmono_mode = DMONO_MAIN;
    MP_VERBOSE(mpctx, "dmono pid[%04x] set to use %s ch.\n", sh->demuxer_id,
               sh->dmono_mode == DMONO_MAIN ? "main"
               : sh->dmono_mode == DMONO_SUB ? "sub" : "both");
}

static void setup_prog(struct MPContext *mpctx)
{
    int r;
    unsigned int progid;
    demux_program_t prog;

    mpctx->progid = -1;

    if (mpctx->opts->progid >= 0)
        progid = mpctx->opts->progid;
    else {
        r = demux_stream_control(mpctx->demuxer, STREAM_CTRL_DVB_GET_SID, &progid);
        if (r != DEMUXER_CTRL_OK)
            return;
    }
    prog.progid = progid;
    prog.vlangs = mpctx->opts->stream_lang[STREAM_VIDEO];
    prog.alangs = mpctx->opts->stream_lang[STREAM_AUDIO];
    prog.slangs = mpctx->opts->stream_lang[STREAM_SUB];
    r = demux_control(mpctx->demuxer, DEMUXER_CTRL_IDENTIFY_PROGRAM, &prog);
    if (r != DEMUXER_CTRL_OK) {
        MP_WARN(mpctx, "failed to set program_id:%d.\n", prog.progid);
        return;
    }

    mpctx->progid = prog.progid;
    if (mpctx->opts->stream_id[0][STREAM_VIDEO] == -1)
        mpctx->opts->stream_id[0][STREAM_VIDEO] = prog.vid;
    if (mpctx->opts->stream_id[0][STREAM_AUDIO] == -1)
        mpctx->opts->stream_id[0][STREAM_AUDIO] = prog.aid;
    if (mpctx->opts->stream_id[0][STREAM_SUB] == -1)
        mpctx->opts->stream_id[0][STREAM_SUB] = prog.sid;
    MP_VERBOSE(mpctx, "selected prog_id:%d.\n", prog.progid);
    return;
}

// Start playing the current playlist entry.
// Handle initialization and deinitialization.
static void play_current_file(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    double playback_start = -1e100;

    mp_notify(mpctx, MPV_EVENT_START_FILE, NULL);

    mp_cancel_reset(mpctx->playback_abort);

    mpctx->error_playing = MPV_ERROR_LOADING_FAILED;
    mpctx->stop_play = 0;
    mpctx->filename = NULL;
    mpctx->shown_aframes = 0;
    mpctx->shown_vframes = 0;
    mpctx->last_vo_pts = MP_NOPTS_VALUE;
    mpctx->last_chapter_seek = -2;
    mpctx->last_chapter_pts = MP_NOPTS_VALUE;
    mpctx->last_chapter = -2;
    mpctx->paused = false;
    mpctx->paused_for_cache = false;
    mpctx->cache_buffer = -1;
    mpctx->playing_msg_shown = false;
    mpctx->max_frames = -1;
    mpctx->video_speed = mpctx->audio_speed = opts->playback_speed;
    mpctx->speed_factor_a = mpctx->speed_factor_v = 1.0;
    mpctx->display_sync_error = 0.0;
    mpctx->display_sync_active = false;
    mpctx->seek = (struct seek_params){ 0 };

    reset_playback_state(mpctx);

    mpctx->playing = mpctx->playlist->current;
    if (!mpctx->playing || !mpctx->playing->filename)
        goto terminate_playback;
    mpctx->playing->reserved += 1;

    mpctx->filename = talloc_strdup(NULL, mpctx->playing->filename);
    mpctx->stream_open_filename = mpctx->filename;

    mpctx->add_osd_seek_info &= OSD_SEEK_INFO_CURRENT_FILE;

    if (opts->reset_options) {
        for (int n = 0; opts->reset_options[n]; n++) {
            const char *opt = opts->reset_options[n];
            if (opt[0]) {
                if (strcmp(opt, "all") == 0) {
                    m_config_backup_all_opts(mpctx->mconfig);
                } else {
                    m_config_backup_opt(mpctx->mconfig, opt);
                }
            }
        }
    }

    mp_load_auto_profiles(mpctx);

    mp_load_playback_resume(mpctx, mpctx->filename);

    load_per_file_options(mpctx->mconfig, mpctx->playing->params,
                          mpctx->playing->num_params);

    mpctx->max_frames = opts->play_frames;

    handle_force_window(mpctx, false);

    MP_INFO(mpctx, "Playing: %s\n", mpctx->filename);

reopen_file:

    assert(mpctx->demuxer == NULL);

    if (process_open_hooks(mpctx) < 0)
        goto terminate_playback;

    if (opts->stream_dump && opts->stream_dump[0]) {
        if (stream_dump(mpctx, mpctx->stream_open_filename) >= 0)
            mpctx->error_playing = 1;
        goto terminate_playback;
    }

    open_demux_reentrant(mpctx);
    if (!mpctx->demuxer || mpctx->stop_play)
        goto terminate_playback;

    if (mpctx->demuxer->playlist) {
        struct playlist *pl = mpctx->demuxer->playlist;
        int entry_stream_flags = 0;
        if (!pl->disable_safety) {
            entry_stream_flags = STREAM_SAFE_ONLY;
            if (mpctx->demuxer->is_network)
                entry_stream_flags |= STREAM_NETWORK_ONLY;
        }
        for (struct playlist_entry *e = pl->first; e; e = e->next)
            e->stream_flags |= entry_stream_flags;
        transfer_playlist(mpctx, pl);
        mp_notify_property(mpctx, "playlist");
        mpctx->error_playing = 2;
        goto terminate_playback;
    }

    if (mpctx->opts->rebase_start_time)
        demux_set_ts_offset(mpctx->demuxer, -mpctx->demuxer->start_time);
    enable_demux_thread(mpctx, mpctx->demuxer);

    load_chapters(mpctx);
    add_demuxer_tracks(mpctx, mpctx->demuxer);

    open_external_files(mpctx, opts->audio_files, STREAM_AUDIO);
    open_external_files(mpctx, opts->sub_name, STREAM_SUB);
    open_external_files(mpctx, opts->external_files, STREAM_TYPE_COUNT);
    autoload_external_files(mpctx);

    check_previous_track_selection(mpctx);

    if (process_preloaded_hooks(mpctx))
        goto terminate_playback;

    if (!init_complex_filters(mpctx))
        goto terminate_playback;

    // If program_id is specified by user-option or by inupt stream (DVB),
    // use it for selecting tracks.
    setup_prog(mpctx);

    assert(NUM_PTRACKS == 2); // opts->stream_id is hardcoded to 2
    for (int t = 0; t < STREAM_TYPE_COUNT; t++) {
        for (int i = 0; i < NUM_PTRACKS; i++) {
            struct track *sel = NULL;
            bool taken = (t == STREAM_VIDEO && mpctx->vo_chain) ||
                         (t == STREAM_AUDIO && mpctx->ao_chain);
            if (!taken)
                sel = select_default_track(mpctx, i, t);
            mpctx->current_track[i][t] = sel;
            mp_select_dmono_sub_ch(mpctx, sel);
        }
    }
    for (int t = 0; t < STREAM_TYPE_COUNT; t++) {
        for (int i = 0; i < NUM_PTRACKS; i++) {
            struct track *track = mpctx->current_track[i][t];
            if (track) {
                if (track->selected) {
                    MP_ERR(mpctx, "Track %d can't be selected twice.\n",
                           track->user_tid);
                    mpctx->current_track[i][t] = NULL;
                } else {
                    track->selected = true;
                }
            }
        }
    }

    for (int n = 0; n < mpctx->num_tracks; n++)
        reselect_demux_stream(mpctx, mpctx->tracks[n]);

    update_demuxer_properties(mpctx);

#if HAVE_ENCODING
    if (mpctx->encode_lavc_ctx && mpctx->current_track[0][STREAM_VIDEO])
        encode_lavc_expect_stream(mpctx->encode_lavc_ctx, AVMEDIA_TYPE_VIDEO);
    if (mpctx->encode_lavc_ctx && mpctx->current_track[0][STREAM_AUDIO])
        encode_lavc_expect_stream(mpctx->encode_lavc_ctx, AVMEDIA_TYPE_AUDIO);
    if (mpctx->encode_lavc_ctx) {
        encode_lavc_set_metadata(mpctx->encode_lavc_ctx,
                                 mpctx->demuxer->metadata);
    }
#endif

    update_playback_speed(mpctx);

    if (!init_complex_filter_decoders(mpctx))
        goto terminate_playback;

    reinit_video_chain(mpctx);
    reinit_audio_chain(mpctx);
    reinit_sub_all(mpctx);

    if (!mpctx->vo_chain && !mpctx->ao_chain) {
        MP_FATAL(mpctx, "No video or audio streams selected.\n");
        mpctx->error_playing = MPV_ERROR_NOTHING_TO_PLAY;
        goto terminate_playback;
    }

    if (mpctx->vo_chain && mpctx->vo_chain->is_coverart) {
        MP_INFO(mpctx,
            "Displaying attached picture. Use --no-audio-display to prevent this.\n");
    }

    if (!mpctx->vo_chain)
        handle_force_window(mpctx, true);

    MP_VERBOSE(mpctx, "Starting playback...\n");

    mpctx->playback_initialized = true;
    mp_notify(mpctx, MPV_EVENT_FILE_LOADED, NULL);
    update_screensaver_state(mpctx);

    if (mpctx->max_frames == 0) {
        if (!mpctx->stop_play)
            mpctx->stop_play = PT_NEXT_ENTRY;
        mpctx->error_playing = 0;
        goto terminate_playback;
    }

    double startpos = rel_time_to_abs(mpctx, opts->play_start);
    if (startpos == MP_NOPTS_VALUE && opts->chapterrange[0] > 0) {
        double start = chapter_start_time(mpctx, opts->chapterrange[0] - 1);
        if (start != MP_NOPTS_VALUE)
            startpos = start;
    }
    if (startpos != MP_NOPTS_VALUE) {
        if (!opts->rebase_start_time) {
            startpos += mpctx->demuxer->start_time;
        }
        queue_seek(mpctx, MPSEEK_ABSOLUTE, startpos, MPSEEK_DEFAULT, 0);
        execute_queued_seek(mpctx);
    }

    update_internal_pause_state(mpctx);

    open_recorder(mpctx, true);

    playback_start = mp_time_sec();
    mpctx->error_playing = 0;
    mpctx->in_playloop = true;
    while (!mpctx->stop_play)
        run_playloop(mpctx);
    mpctx->in_playloop = false;

    MP_VERBOSE(mpctx, "EOF code: %d  \n", mpctx->stop_play);

terminate_playback:

    update_core_idle_state(mpctx);

    process_unload_hooks(mpctx);

    if (mpctx->stop_play == KEEP_PLAYING)
        mpctx->stop_play = AT_END_OF_FILE;

    if (mpctx->stop_play != AT_END_OF_FILE)
        clear_audio_output_buffers(mpctx);

    if (mpctx->step_frames)
        opts->pause = 1;

    mp_abort_playback_async(mpctx);

    close_recorder(mpctx);

    // time to uninit all, except global stuff:
    uninit_complex_filters(mpctx);
    uninit_audio_chain(mpctx);
    uninit_video_chain(mpctx);
    uninit_sub_all(mpctx);
    uninit_demuxer(mpctx);
    if (!opts->gapless_audio && !mpctx->encode_lavc_ctx)
        uninit_audio_out(mpctx);

    mpctx->playback_initialized = false;

    if (mpctx->stop_play == PT_RELOAD_FILE) {
        mpctx->stop_play = KEEP_PLAYING;
        mp_cancel_reset(mpctx->playback_abort);
        goto reopen_file;
    }

    m_config_restore_backups(mpctx->mconfig);

    talloc_free(mpctx->filtered_tags);
    mpctx->filtered_tags = NULL;

    mp_notify(mpctx, MPV_EVENT_TRACKS_CHANGED, NULL);

    bool nothing_played = !mpctx->shown_aframes && !mpctx->shown_vframes &&
                          mpctx->error_playing <= 0;
    struct mpv_event_end_file end_event = {0};
    switch (mpctx->stop_play) {
    case PT_ERROR:
    case AT_END_OF_FILE:
    {
        if (mpctx->error_playing == 0 && nothing_played)
            mpctx->error_playing = MPV_ERROR_NOTHING_TO_PLAY;
        if (mpctx->error_playing < 0) {
            end_event.error = mpctx->error_playing;
            end_event.reason = MPV_END_FILE_REASON_ERROR;
        } else if (mpctx->error_playing == 2) {
            end_event.reason = MPV_END_FILE_REASON_REDIRECT;
        } else {
            end_event.reason = MPV_END_FILE_REASON_EOF;
        }
        if (mpctx->playing) {
            // Played/paused for longer than 1 second -> ok
            mpctx->playing->playback_short =
                playback_start < 0 || mp_time_sec() - playback_start < 1.0;
            mpctx->playing->init_failed = nothing_played;
        }
        break;
    }
    // Note that error_playing is meaningless in these cases.
    case PT_NEXT_ENTRY:
    case PT_CURRENT_ENTRY:
    case PT_STOP:           end_event.reason = MPV_END_FILE_REASON_STOP; break;
    case PT_QUIT:           end_event.reason = MPV_END_FILE_REASON_QUIT; break;
    };
    mp_notify(mpctx, MPV_EVENT_END_FILE, &end_event);

    MP_VERBOSE(mpctx, "finished playback, %s (reason %d)\n",
               mpv_error_string(end_event.error), end_event.reason);
    if (end_event.error == MPV_ERROR_UNKNOWN_FORMAT)
        MP_ERR(mpctx, "Failed to recognize file format.\n");
    MP_INFO(mpctx, "\n");

    if (mpctx->playing)
        playlist_entry_unref(mpctx->playing);
    mpctx->playing = NULL;
    talloc_free(mpctx->filename);
    mpctx->filename = NULL;
    mpctx->stream_open_filename = NULL;

    if (end_event.error < 0 && nothing_played) {
        mpctx->files_broken++;
    } else if (end_event.error < 0) {
        mpctx->files_errored++;
    } else {
        mpctx->files_played++;
    }
}

// Determine the next file to play. Note that if this function returns non-NULL,
// it can have side-effects and mutate mpctx.
//  direction: -1 (previous) or +1 (next)
//  force: if true, don't skip playlist entries marked as failed
//  mutate: if true, change loop counters
struct playlist_entry *mp_next_file(struct MPContext *mpctx, int direction,
                                    bool force, bool mutate)
{
    struct playlist_entry *next = playlist_get_next(mpctx->playlist, direction);
    if (next && direction < 0 && !force) {
        // Don't jump to files that would immediately go to next file anyway
        while (next && next->playback_short)
            next = next->prev;
        // Always allow jumping to first file
        if (!next && mpctx->opts->loop_times == 1)
            next = mpctx->playlist->first;
    }
    if (!next && mpctx->opts->loop_times != 1) {
        if (direction > 0) {
            if (mpctx->opts->shuffle)
                playlist_shuffle(mpctx->playlist);
            next = mpctx->playlist->first;
            if (next && mpctx->opts->loop_times > 1)
                mpctx->opts->loop_times--;
        } else {
            next = mpctx->playlist->last;
            // Don't jump to files that would immediately go to next file anyway
            while (next && next->playback_short)
                next = next->prev;
        }
        bool ignore_failures = mpctx->opts->loop_times == -2;
        if (!force && next && next->init_failed && !ignore_failures) {
            // Don't endless loop if no file in playlist is playable
            bool all_failed = true;
            struct playlist_entry *cur;
            for (cur = mpctx->playlist->first; cur; cur = cur->next) {
                all_failed &= cur->init_failed;
                if (!all_failed)
                    break;
            }
            if (all_failed)
                next = NULL;
        }
    }
    return next;
}

// Play all entries on the playlist, starting from the current entry.
// Return if all done.
void mp_play_files(struct MPContext *mpctx)
{
    prepare_playlist(mpctx, mpctx->playlist);

    for (;;) {
        idle_loop(mpctx);
        if (mpctx->stop_play == PT_QUIT)
            break;

        play_current_file(mpctx);
        if (mpctx->stop_play == PT_QUIT)
            break;

        struct playlist_entry *new_entry = mpctx->playlist->current;
        if (mpctx->stop_play == PT_NEXT_ENTRY || mpctx->stop_play == PT_ERROR ||
            mpctx->stop_play == AT_END_OF_FILE || !mpctx->stop_play)
        {
            new_entry = mp_next_file(mpctx, +1, false, true);
        }

        mpctx->playlist->current = new_entry;
        mpctx->playlist->current_was_replaced = false;
        mpctx->stop_play = 0;

        if (!mpctx->playlist->current && mpctx->opts->player_idle_mode < 2)
            break;
    }

    cancel_open(mpctx);
}

// Abort current playback and set the given entry to play next.
// e must be on the mpctx->playlist.
void mp_set_playlist_entry(struct MPContext *mpctx, struct playlist_entry *e)
{
    assert(!e || playlist_entry_to_index(mpctx->playlist, e) >= 0);
    mpctx->playlist->current = e;
    mpctx->playlist->current_was_replaced = false;
    if (!mpctx->stop_play)
        mpctx->stop_play = PT_CURRENT_ENTRY;
    mp_wakeup_core(mpctx);
}

static void set_track_recorder_sink(struct track *track,
                                    struct mp_recorder_sink *sink)
{
    if (track->d_sub)
        sub_set_recorder_sink(track->d_sub, sink);
    if (track->d_video)
        track->d_video->recorder_sink = sink;
    if (track->d_audio)
        track->d_audio->recorder_sink = sink;
    track->remux_sink = sink;
}

void close_recorder(struct MPContext *mpctx)
{
    if (!mpctx->recorder)
        return;

    for (int n = 0; n < mpctx->num_tracks; n++)
        set_track_recorder_sink(mpctx->tracks[n], NULL);

    mp_recorder_destroy(mpctx->recorder);
    mpctx->recorder = NULL;
}

// Like close_recorder(), but also unset the option. Intended for use on errors.
void close_recorder_and_error(struct MPContext *mpctx)
{
    close_recorder(mpctx);
    talloc_free(mpctx->opts->record_file);
    mpctx->opts->record_file = NULL;
    mp_notify_property(mpctx, "record-file");
    MP_ERR(mpctx, "Disabling stream recording.\n");
}

void open_recorder(struct MPContext *mpctx, bool on_init)
{
    if (!mpctx->playback_initialized)
        return;

    close_recorder(mpctx);

    char *target = mpctx->opts->record_file;
    if (!target || !target[0])
        return;

    struct sh_stream **streams = NULL;
    int num_streams = 0;

    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *track = mpctx->tracks[n];
        if (track->stream && track->selected &&
            (track->d_sub || track->d_video || track->d_audio))
        {
            MP_TARRAY_APPEND(NULL, streams, num_streams, track->stream);
        }
    }

    mpctx->recorder = mp_recorder_create(mpctx->global, mpctx->opts->record_file,
                                         streams, num_streams);

    if (!mpctx->recorder) {
        talloc_free(streams);
        close_recorder_and_error(mpctx);
        return;
    }

    if (!on_init)
        mp_recorder_mark_discontinuity(mpctx->recorder);

    int n_stream = 0;
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *track = mpctx->tracks[n];
        if (n_stream >= num_streams)
            break;
        // (We expect track->stream not to be reused on other tracks.)
        if (track->stream == streams[n_stream]) {
            set_track_recorder_sink(track,
                            mp_recorder_get_sink(mpctx->recorder, n_stream));
            n_stream++;
        }
    }

    talloc_free(streams);
}

