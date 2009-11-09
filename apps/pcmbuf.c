/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2005 by Miika Pekkarinen
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdio.h>
#include "config.h"
#include "system.h"
#include "debug.h"
#include <kernel.h>
#include "pcmbuf.h"
#include "pcm.h"
#include "playback.h"

/* Define LOGF_ENABLE to enable logf output in this file */
/*#define LOGF_ENABLE*/
#include "logf.h"
#ifndef SIMULATOR
#include "cpu.h"
#endif
#include <string.h>
#include "settings.h"
#include "audio.h"
#include "dsp.h"

#define PCMBUF_TARGET_CHUNK 32768 /* This is the target fill size of chunks
                                     on the pcm buffer */
#define PCMBUF_MINAVG_CHUNK 24576 /* This is the minimum average size of
                                     chunks on the pcm buffer (or we run out
                                     of buffer descriptors, which is
                                     non-fatal) */
#define PCMBUF_MIN_CHUNK     4096 /* We try to never feed a chunk smaller than
                                     this to the DMA */
#define PCMBUF_MIX_CHUNK     8192 /* This is the maximum size of one packet
                                     for mixing (crossfade or voice) */

#if MEMORYSIZE > 2
/* Keep watermark high for iPods at least (2s) */
#define PCMBUF_WATERMARK     (NATIVE_FREQUENCY * 4 * 2)
#else
#define PCMBUF_WATERMARK     (NATIVE_FREQUENCY * 1) /* 0.25 seconds */
#endif

/* Structure we can use to queue pcm chunks in memory to be played
 * by the driver code. */
struct chunkdesc
{
    void *addr;
    size_t size;
    struct chunkdesc* link;
    /* true if last chunk in the track */
    bool end_of_track;
};

#define CHUNK_DESCS(bufsize) \
    ((bufsize) / PCMBUF_MINAVG_CHUNK)
#define CHUNK_DESCS_SIZE(bufsize) \
    (CHUNK_DESCS(bufsize)*sizeof(struct chunkdesc))

/* Size of the PCM buffer. */
static size_t pcmbuf_size IDATA_ATTR = 0;
static char *pcmbuf_bufend IDATA_ATTR;
static char *pcmbuffer IDATA_ATTR;
/* Current PCM buffer write index. */
static size_t pcmbuffer_pos IDATA_ATTR;
/* Amount pcmbuffer_pos will be increased.*/
static size_t pcmbuffer_fillpos IDATA_ATTR;
static char *fadebuf IDATA_ATTR;
static char *voicebuf IDATA_ATTR;

static bool end_of_track IDATA_ATTR;
bool track_transition IDATA_ATTR;

/* Crossfade related state */
static bool crossfade_enabled;
static bool crossfade_enable_request;
static bool crossfade_mixmode;
static bool crossfade_active IDATA_ATTR;
static bool crossfade_init IDATA_ATTR;

/* Track the current location for processing crossfade */
static struct chunkdesc *crossfade_chunk IDATA_ATTR;
#ifdef HAVE_CROSSFADE
static size_t crossfade_sample IDATA_ATTR;

/* Counters for fading in new data */
static size_t crossfade_fade_in_total IDATA_ATTR;
static size_t crossfade_fade_in_rem IDATA_ATTR;
#endif

static struct chunkdesc *read_chunk IDATA_ATTR;
static struct chunkdesc *read_end_chunk IDATA_ATTR;
static struct chunkdesc *write_chunk IDATA_ATTR;
static struct chunkdesc *write_end_chunk IDATA_ATTR;
static size_t last_chunksize IDATA_ATTR;

static size_t pcmbuf_unplayed_bytes IDATA_ATTR;
static size_t pcmbuf_watermark IDATA_ATTR;

static struct chunkdesc *mix_chunk IDATA_ATTR;
static size_t pcmbuf_mix_sample IDATA_ATTR;

static bool low_latency_mode = false;
static bool pcmbuf_flush;

#ifdef HAVE_PRIORITY_SCHEDULING
static int codec_thread_priority = PRIORITY_PLAYBACK;
#endif

extern unsigned int codec_thread_id;

/* Helpful macros for use in conditionals this assumes some of the above
 * static variable names */
#define NEED_FLUSH(position) \
    (pcmbuffer_fillpos > PCMBUF_TARGET_CHUNK || position >= pcmbuf_size)
#define LOW_DATA(quarter_secs) \
    (pcmbuf_unplayed_bytes < NATIVE_FREQUENCY * quarter_secs)

static void pcmbuf_finish_track_change(void);
#ifdef HAVE_CROSSFADE
static void crossfade_start(void);
static void flush_crossfade(char *buf, size_t length);
#endif
static bool pcmbuf_crossfade_init(bool manual_skip);
static void pcmbuf_finish_crossfade_enable(void);
static bool pcmbuf_is_crossfade_enabled(void);


/**************************************/

/* define this to show detailed chunkdesc usage information on the sim console */
/*#define DESC_DEBUG*/

#ifndef SIMULATOR
#undef DESC_DEBUG
#endif
#ifdef DESC_DEBUG
static struct chunkdesc *first_desc;
static bool show_desc_in_use = false;
#define DISPLAY_DESC(caller) while(!show_desc(caller))
#define DESC_IDX(desc)  (desc ? desc - first_desc : -1)
#define DESCL_IDX(desc) (desc && desc->link ? desc->link - first_desc : -1)
#define SHOW_1ST(desc) if(DESC_IDX (desc)==-1) DEBUGF(" -- "); \
                       else DEBUGF(" %02d ",   DESC_IDX(desc))
#define SHOW_2ND(desc) if(DESCL_IDX(desc)==-1) DEBUGF("l --  "); \
                       else DEBUGF("l %02d  ", DESCL_IDX(desc))
#define DESC_SHOW(tag, desc) DEBUGF(tag);SHOW_1ST(desc); \
                             DEBUGF(tag);SHOW_2ND(desc)

static bool show_desc(char *caller)
{
    if (show_desc_in_use) return false;
    show_desc_in_use = true;
    DEBUGF("%-14s\t", caller);
    DESC_SHOW("r",  read_chunk);
    DESC_SHOW("re", read_end_chunk);
    DEBUGF(" ");
    DESC_SHOW("w",  write_chunk);
    DESC_SHOW("we", write_end_chunk);
    DEBUGF("\n");
    show_desc_in_use = false;
    return true;
}
#else
#define DISPLAY_DESC(caller) do{}while(0)
#endif


/* Commit PCM data */

/* This is really just part of pcmbuf_flush_fillpos, but is easier to keep
 * in a separate function for the moment */
static inline void pcmbuf_add_chunk(void)
{
    register size_t size = pcmbuffer_fillpos;
    /* Grab the next description to write, and change the write pointer */
    register struct chunkdesc *pcmbuf_current = write_chunk;
    write_chunk = pcmbuf_current->link;
    /* Fill in the values in the new buffer chunk */
    pcmbuf_current->addr = &pcmbuffer[pcmbuffer_pos];
    pcmbuf_current->size = size;
    pcmbuf_current->end_of_track = end_of_track;
    pcmbuf_current->link = NULL;
    end_of_track = false;   /* This is single use only */
    if (read_chunk != NULL) {
        if (pcmbuf_flush)
        {
            write_end_chunk->link = read_chunk->link;
            read_chunk->link = pcmbuf_current;
            while (write_end_chunk->link)
            {
                write_end_chunk = write_end_chunk->link;
                pcmbuf_unplayed_bytes -= write_end_chunk->size;
            }
            pcmbuf_flush = false;
        }
        /* If there is already a read buffer setup, add to it */
        else
            read_end_chunk->link = pcmbuf_current;
    } else {
        /* Otherwise create the buffer */
        read_chunk = pcmbuf_current;
    }
    /* This is now the last buffer to read */
    read_end_chunk = pcmbuf_current;

    /* Update bytes counters */
    pcmbuf_unplayed_bytes += size;

    pcmbuffer_pos += size;
    if (pcmbuffer_pos >= pcmbuf_size)
        pcmbuffer_pos -= pcmbuf_size;

    pcmbuffer_fillpos = 0;
    DISPLAY_DESC("add_chunk");
}

/**
 * Commit samples waiting to the pcm buffer.
 */
static bool pcmbuf_flush_fillpos(void)
{
    if (pcmbuffer_fillpos) {
        /* Never use the last buffer descriptor */
        while (write_chunk == write_end_chunk) {
            /* If this happens, something is being stupid */
            if (!pcm_is_playing()) {
                logf("pcmbuf_flush_fillpos error");
                pcmbuf_play_start();
            }
            /* Let approximately one chunk of data playback */
            sleep(HZ*PCMBUF_TARGET_CHUNK/(NATIVE_FREQUENCY*4));
        }
        pcmbuf_add_chunk();
        return true;
    }
    return false;
}

#ifdef HAVE_PRIORITY_SCHEDULING
static void boost_codec_thread(bool boost)
{
    /* Keep voice and codec threads at the same priority or else voice
     * will starve if the codec thread's priority is boosted. */
    if (boost)
    {
        int priority = (PRIORITY_PLAYBACK - PRIORITY_PLAYBACK_MAX)*pcmbuf_unplayed_bytes
                          / (2*NATIVE_FREQUENCY) + PRIORITY_PLAYBACK_MAX;

        if (priority != codec_thread_priority)
        {
            codec_thread_priority = priority;
            thread_set_priority(codec_thread_id, priority);
            voice_thread_set_priority(priority);
        }
    }
    else if (codec_thread_priority != PRIORITY_PLAYBACK)
    {
        thread_set_priority(codec_thread_id, PRIORITY_PLAYBACK);
        voice_thread_set_priority(PRIORITY_PLAYBACK);
        codec_thread_priority = PRIORITY_PLAYBACK;
    }
}
#else
#define boost_codec_thread(boost) do{}while(0)
#endif /* HAVE_PRIORITY_SCHEDULING */

static bool prepare_insert(size_t length)
{
    if (low_latency_mode)
    {
        /* 1/4s latency. */
        if (!LOW_DATA(1) && pcm_is_playing())
            return false;
    }

    /* Need to save PCMBUF_MIN_CHUNK to prevent wrapping overwriting */
    if (pcmbuf_free() < length + PCMBUF_MIN_CHUNK)
        return false;

    /* boost CPU if needed to either fill to watermark or for pre-buffer */
    if (pcm_is_playing())
    {
        /* Only codec thread initiates boost - voice boosts the cpu when playing
           a clip */
#ifndef SIMULATOR
        if (thread_get_current() == codec_thread_id)
#endif /* SIMULATOR */
        {
            if (pcmbuf_unplayed_bytes <= pcmbuf_watermark)
            {
                /* Fill PCM buffer by boosting cpu */
                trigger_cpu_boost();
                /* If buffer is critically low, override UI priority, else
                   set back to the original priority. */
                boost_codec_thread(LOW_DATA(2));
            }
            else
            {
                boost_codec_thread(false);
            }
        }

        /* Disable crossfade if < .5s of audio */
        if (LOW_DATA(2))
        {
            crossfade_active = false;
        }
    }
    else    /* pcm_is_playing */
    {
        trigger_cpu_boost();

        /* Pre-buffer up to watermark */
#if MEMORYSIZE > 2
        if (!LOW_DATA(4))
#else
        if (pcmbuf_unplayed_bytes > pcmbuf_watermark)
#endif
        {
            logf("pcm starting");
            if (!(audio_status() & AUDIO_STATUS_PAUSE))
                pcmbuf_play_start();
        }
    }

    return true;
}

void *pcmbuf_request_buffer(int *count)
{
#ifdef HAVE_CROSSFADE
    if (crossfade_init)
        crossfade_start();
#endif

    if (crossfade_active) {
        *count = MIN(*count, PCMBUF_MIX_CHUNK/4);
        return fadebuf;
    }
    else
    {
        if(prepare_insert(*count << 2))
        {
            size_t pcmbuffer_index = pcmbuffer_pos + pcmbuffer_fillpos;
            if (pcmbuf_size - pcmbuffer_index >= PCMBUF_MIN_CHUNK)
            {
                /* Usual case, there's space here */
                return &pcmbuffer[pcmbuffer_index];
            }
            else
            {
                /* Flush and wrap the buffer */
                pcmbuf_flush_fillpos();
                pcmbuffer_pos = 0;
                return &pcmbuffer[0];
            }
        }
        else
        {
            return NULL;
        }
    }
}

void pcmbuf_write_complete(int count)
{
    size_t length = (size_t)(unsigned int)count << 2;
#ifdef HAVE_CROSSFADE
    if (crossfade_active)
    {
        flush_crossfade(fadebuf, length);
        if (!(crossfade_fade_in_rem || crossfade_chunk))
            crossfade_active = false;
    }
    else
#endif    
    {
        pcmbuffer_fillpos += length;

        if (NEED_FLUSH(pcmbuffer_pos + pcmbuffer_fillpos))
            pcmbuf_flush_fillpos();
    }
}


/* Init */

static void pcmbuf_init_pcmbuffers(void)
{
#ifdef DESC_DEBUG
    first_desc = write_chunk;
#endif
    struct chunkdesc *next = write_chunk;
    next++;
    write_end_chunk = write_chunk;
    while ((void *)next < (void *)pcmbuf_bufend) {
        write_end_chunk->link=next;
        write_end_chunk=next;
        next++;
    }
    DISPLAY_DESC("init");
}

static size_t pcmbuf_get_next_required_pcmbuf_size(void)
{
    size_t seconds = 1;

    if (crossfade_enable_request)
        seconds += global_settings.crossfade_fade_out_delay
                   + global_settings.crossfade_fade_out_duration;

#if MEMORYSIZE > 2
    /* Buffer has to be at least 2s long. */
    seconds += 2;
#endif
    logf("pcmbuf len: %ld", (long)seconds);
    return seconds * (NATIVE_FREQUENCY*4); /* 2 channels + 2 bytes/sample */
}

static char *pcmbuf_calc_pcmbuffer_ptr(size_t bufsize)
{
    return pcmbuf_bufend - (bufsize + PCMBUF_MIX_CHUNK * 2 +
               CHUNK_DESCS_SIZE(bufsize));
}

/* Initialize the pcmbuffer the structure looks like this:
 * ...|---------PCMBUF---------|FADEBUF|VOICEBUF|DESCS|... */
size_t pcmbuf_init(unsigned char *bufend)
{
    pcmbuf_bufend = bufend;
    pcmbuf_size = pcmbuf_get_next_required_pcmbuf_size();
    pcmbuffer = pcmbuf_calc_pcmbuffer_ptr(pcmbuf_size);
    fadebuf = &pcmbuffer[pcmbuf_size];
    voicebuf = &fadebuf[PCMBUF_MIX_CHUNK];
    write_chunk = (struct chunkdesc *)&voicebuf[PCMBUF_MIX_CHUNK];

    pcmbuf_init_pcmbuffers();

    if(track_transition){logf("pcmbuf: (init) track transition false");}
    end_of_track = false;
    track_transition = false;

    pcmbuf_finish_crossfade_enable();

    pcmbuf_play_stop();

    return pcmbuf_bufend - pcmbuffer;
}


/* Playback */

/** PCM driver callback
 * This function has 3 major logical parts (separated by brackets both for
 * readability and variable scoping).  The first part performs the
 * operations related to finishing off the last buffer we fed to the DMA.
 * The second part detects the end of playlist condition when the pcm
 * buffer is empty except for uncommitted samples.  Then they are committed.
 * The third part performs the operations involved in sending a new buffer
 * to the DMA. */
static void pcmbuf_pcm_callback(unsigned char** start, size_t* size) ICODE_ATTR;
static void pcmbuf_pcm_callback(unsigned char** start, size_t* size)
{
    {
        struct chunkdesc *pcmbuf_current = read_chunk;
        /* Take the finished buffer out of circulation */
        read_chunk = pcmbuf_current->link;

        /* if during a track transition, update the elapsed time */
        if (track_transition)
            audio_pcmbuf_position_callback(last_chunksize);
        
        /* if last buffer in the track, let the audio thread know */
        if (pcmbuf_current->end_of_track)
            pcmbuf_finish_track_change();

        /* Put the finished buffer back into circulation */
        write_end_chunk->link = pcmbuf_current;
        write_end_chunk = pcmbuf_current;

        /* If we've read over the mix chunk while it's still mixing there */
        if (pcmbuf_current == mix_chunk)
            mix_chunk = NULL;
        /* If we've read over the crossfade chunk while it's still fading */
        if (pcmbuf_current == crossfade_chunk)
            crossfade_chunk = read_chunk;
    }
    
    {
        /* Commit last samples at end of playlist */
        if (pcmbuffer_fillpos && !read_chunk)
        {
            logf("pcmbuf_pcm_callback: commit last samples");
            pcmbuf_flush_fillpos();
        }
    }

    {
        /* Send the new buffer to the pcm */
        struct chunkdesc *pcmbuf_new = read_chunk;
        size_t *realsize = size;
        unsigned char** realstart = start;
        if(pcmbuf_new)
        {
            size_t current_size = pcmbuf_new->size;

            pcmbuf_unplayed_bytes -= current_size;
            last_chunksize = current_size;
            *realsize = current_size;
            *realstart = pcmbuf_new->addr;
        }
        else
        {
            /* No more buffers */
            last_chunksize = 0;
            *realsize = 0;
            *realstart = NULL;
            if (end_of_track)
                pcmbuf_finish_track_change();
        }
    }
    DISPLAY_DESC("callback");
}

/* Force playback. */
void pcmbuf_play_start(void)
{
    if (!pcm_is_playing() && pcmbuf_unplayed_bytes && read_chunk != NULL)
    {
        last_chunksize = read_chunk->size;
        pcmbuf_unplayed_bytes -= last_chunksize;
        pcm_play_data(pcmbuf_pcm_callback,
            (unsigned char *)read_chunk->addr, last_chunksize);
    }
}

void pcmbuf_play_stop(void)
{
    pcm_play_stop();

    pcmbuf_unplayed_bytes = 0;
    mix_chunk = NULL;
    if (read_chunk) {
        write_end_chunk->link = read_chunk;
        write_end_chunk = read_end_chunk;
        read_chunk = read_end_chunk = NULL;
    }
    pcmbuffer_pos = 0;
    pcmbuffer_fillpos = 0;
    crossfade_init = false;
    crossfade_active = false;
    pcmbuf_flush = false;
    DISPLAY_DESC("play_stop");

    /* Can unboost the codec thread here no matter who's calling */
    boost_codec_thread(false);
}

void pcmbuf_pause(bool pause)
{
    if (pcm_is_playing())
        pcm_play_pause(!pause);
    else if (!pause)
        pcmbuf_play_start();
}


/* Track change */

/* The codec is moving on to the next track, but the current track is
 * still playing.  Set flags to make sure the elapsed time of the current
 * track is updated properly, and mark the currently written chunk as the
 * last one in the track. */
static void pcmbuf_gapless_track_change(void)
{
    /* we're starting a track transition */
    track_transition = true;
    
    /* mark the last chunk in the track */
    end_of_track = true;
}

static void pcmbuf_crossfade_track_change(void)
{
    /* Initiate automatic crossfade mode */
    pcmbuf_crossfade_init(false);
    /* Notify the wps that the track change starts now */
    audio_post_track_change(false);
}

void pcmbuf_start_track_change(bool manual_skip)
{
    /* Manual track change (always crossfade or flush audio). */
    if (manual_skip)
    {
        pcmbuf_crossfade_init(true);
        audio_post_track_change(false);
    }
    /* Automatic track change w/crossfade, if not in "Track Skip Only" mode. */
    else if (pcmbuf_is_crossfade_enabled() && !pcmbuf_is_crossfade_active()
             && global_settings.crossfade != CROSSFADE_ENABLE_TRACKSKIP)
    {
        if (global_settings.crossfade == CROSSFADE_ENABLE_SHUFFLE_AND_TRACKSKIP)
        {
            if (global_settings.playlist_shuffle)
                /* shuffle mode is on, so crossfade: */
                pcmbuf_crossfade_track_change();
            else
                /* shuffle mode is off, so normal gapless playback */
                pcmbuf_gapless_track_change();
        }
        else
            /* normal crossfade:  */
            pcmbuf_crossfade_track_change();
    }
    else
        /* normal gapless playback. */
        pcmbuf_gapless_track_change();
}

/* Called when the last chunk in the track has been played */
static void pcmbuf_finish_track_change(void)
{
    /* not in a track transition anymore */
    if(track_transition){logf("pcmbuf: (finish change) track transition false");}
    track_transition = false;
    
    /* notify playback that the track has just finished */
    audio_post_track_change(true);
}


/* Crossfade */

/* Clip sample to signed 16 bit range */
static inline int32_t clip_sample_16(int32_t sample)
{
    if ((int16_t)sample != sample)
        sample = 0x7fff ^ (sample >> 31);
    return sample;
}

/** 
 * Low memory targets don't have crossfade, so don't compile crossfade
 * specific code in order to save some memory.                         */

#ifdef HAVE_CROSSFADE
/**
 * Completely process the crossfade fade out effect with current pcm buffer.
 */
static void crossfade_process_buffer(size_t fade_in_delay,
        size_t fade_out_delay, size_t fade_out_rem)
{
    if (!crossfade_mixmode)
    {
        /* Fade out the specified amount of the already processed audio */
        size_t total_fade_out = fade_out_rem;
        size_t fade_out_sample;
        struct chunkdesc *fade_out_chunk = crossfade_chunk;

        /* Find the right chunk to start fading out */
        fade_out_delay += crossfade_sample * 2;
        while (fade_out_delay != 0 && fade_out_delay >= fade_out_chunk->size)
        {
            fade_out_delay -= fade_out_chunk->size;
            fade_out_chunk = fade_out_chunk->link;
        }
        /* The start sample within the chunk */
        fade_out_sample = fade_out_delay / 2;

        while (fade_out_rem > 0)
        {
            /* Each 1/10 second of audio will have the same fade applied */
            size_t block_rem = MIN(NATIVE_FREQUENCY * 4 / 10, fade_out_rem);
            int factor = (fade_out_rem << 8) / total_fade_out;

            fade_out_rem -= block_rem;

            /* Fade this block */
            while (block_rem > 0 && fade_out_chunk != NULL)
            {
                /* Fade one sample */
                int16_t *buf = (int16_t *)fade_out_chunk->addr;
                int32_t sample = buf[fade_out_sample];
                buf[fade_out_sample++] = (sample * factor) >> 8;

                block_rem -= 2;
                /* Move to the next chunk as needed */
                if (fade_out_sample * 2 >= fade_out_chunk->size)
                {
                    fade_out_chunk = fade_out_chunk->link;
                    fade_out_sample = 0;
                }
            }
        }
    }

    /* Find the right chunk and sample to start fading in */
    fade_in_delay += crossfade_sample * 2;
    while (fade_in_delay != 0 && fade_in_delay >= crossfade_chunk->size)
    {
        fade_in_delay -= crossfade_chunk->size;
        crossfade_chunk = crossfade_chunk->link;
    }
    crossfade_sample = fade_in_delay / 2;
    logf("process done!");
}

/* Initializes crossfader, calculates all necessary parameters and
 * performs fade-out with the pcm buffer.  */
static void crossfade_start(void)
{
    size_t crossfade_rem;
    size_t crossfade_need;
    size_t fade_out_rem;
    size_t fade_out_delay;
    size_t fade_in_delay;

    crossfade_init = false;
    /* Reject crossfade if less than .5s of data */
    if (LOW_DATA(2)) {
        logf("crossfade rejected");
        pcmbuf_play_stop();
        return ;
    }

    logf("crossfade_start");
    pcmbuf_flush_fillpos();
    crossfade_active = true;

    /* Initialize the crossfade buffer size to all of the buffered data that
     * has not yet been sent to the DMA */
    crossfade_rem = pcmbuf_unplayed_bytes;
    crossfade_chunk = read_chunk->link;
    crossfade_sample = 0;

    /* Get fade out delay from settings. */
    fade_out_delay =
        NATIVE_FREQUENCY * global_settings.crossfade_fade_out_delay * 4;

    /* Get fade out duration from settings. */
    fade_out_rem =
        NATIVE_FREQUENCY * global_settings.crossfade_fade_out_duration * 4;

    crossfade_need = fade_out_delay + fade_out_rem;
    /* We want only to modify the last part of the buffer. */
    if (crossfade_rem > crossfade_need)
    {
        size_t crossfade_extra = crossfade_rem - crossfade_need;
        while (crossfade_extra > crossfade_chunk->size)
        {
            crossfade_extra -= crossfade_chunk->size;
            crossfade_chunk = crossfade_chunk->link;
        }
        crossfade_sample = crossfade_extra / 2;
    }
    /* Truncate fade out duration if necessary. */
    else if (crossfade_rem < crossfade_need)
    {
        size_t crossfade_short = crossfade_need - crossfade_rem;
        if (fade_out_rem >= crossfade_short)
            fade_out_rem -= crossfade_short;
        else
        {
            fade_out_delay -= crossfade_short - fade_out_rem;
            fade_out_rem = 0;
        }
    }

    /* Get also fade in duration and delays from settings. */
    crossfade_fade_in_total =
        NATIVE_FREQUENCY * global_settings.crossfade_fade_in_duration * 4;
    crossfade_fade_in_rem = crossfade_fade_in_total;

    fade_in_delay =
        NATIVE_FREQUENCY * global_settings.crossfade_fade_in_delay * 4;

    crossfade_process_buffer(fade_in_delay, fade_out_delay, fade_out_rem);
}

/* Returns the number of bytes _NOT_ mixed */
static size_t crossfade_mix(int factor, const char *buf, size_t length)
{
    const int16_t *input_buf = (const int16_t *)buf;
    int16_t *output_buf = (int16_t *)(crossfade_chunk->addr);
    int16_t *chunk_end = SKIPBYTES(output_buf, crossfade_chunk->size);
    output_buf = &output_buf[crossfade_sample];
    int32_t sample;

    while (length)
    {
        /* fade left and right channel at once to keep buffer alignment */
        int i;
        for (i = 0; i < 2; i++)
        {
            sample = *input_buf++;
            sample = ((sample * factor) >> 8) + *output_buf;
            *output_buf++ = clip_sample_16(sample);
        }

        length -= 4; /* 2 samples, each 16 bit -> 4 bytes */

        if (output_buf >= chunk_end)
        {
            crossfade_chunk = crossfade_chunk->link;
            if (!crossfade_chunk)
                return length;
            output_buf = (int16_t *)crossfade_chunk->addr;
            chunk_end = SKIPBYTES(output_buf, crossfade_chunk->size);
        }
    }
    crossfade_sample = output_buf - (int16_t *)crossfade_chunk->addr;
    return 0;
}

static void flush_crossfade(char *buf, size_t length)
{
    if (length)
    {
        if (crossfade_fade_in_rem)
        {
            size_t samples;
            int16_t *input_buf;

            /* Fade factor for this packet */
            int factor =
                ((crossfade_fade_in_total - crossfade_fade_in_rem) << 8) /
                crossfade_fade_in_total;
            /* Bytes to fade */
            size_t fade_rem = MIN(length, crossfade_fade_in_rem);

            /* We _will_ fade this many bytes */
            crossfade_fade_in_rem -= fade_rem;

            if (crossfade_chunk)
            {
                /* Mix the data */
                size_t fade_total = fade_rem;
                fade_rem = crossfade_mix(factor, buf, fade_rem);
                length -= fade_total - fade_rem;
                buf += fade_total - fade_rem;
                if (!length)
                    return;
            }

            samples = fade_rem / 2;
            input_buf = (int16_t *)buf;
            /* Fade remaining samples in place */
            while (samples--)
            {
                int32_t sample = *input_buf;
                *input_buf++ = (sample * factor) >> 8;
            }
        }

        if (crossfade_chunk)
        {
            /* Mix the data */
            size_t mix_total = length;
            length = crossfade_mix(256, buf, length);
            buf += mix_total - length;
            if (!length)
                return;
        }

        /* Flush samples to the buffer */
        while (!prepare_insert(length))
            sleep(1);
        while (length > 0)
        {
            size_t pcmbuffer_index = pcmbuffer_pos + pcmbuffer_fillpos;
            if (NEED_FLUSH(pcmbuffer_index))
            {
                pcmbuf_flush_fillpos();
                pcmbuffer_index = pcmbuffer_pos + pcmbuffer_fillpos;
            }
            size_t copy_n = MIN(length, pcmbuf_size - pcmbuffer_index);
            memcpy(&pcmbuffer[pcmbuffer_index], buf, copy_n);
            buf += copy_n;
            pcmbuffer_fillpos += copy_n;
            length -= copy_n;
        }
    }
}
#endif /* HAVE_CROSSFADE */

static bool pcmbuf_crossfade_init(bool manual_skip)
{
    /* Can't do two crossfades at once and, no fade if pcm is off now */
    if (crossfade_init || crossfade_active || !pcm_is_playing())
    {
        pcmbuf_play_stop();
        return false;
    }

    trigger_cpu_boost();

    /* Not enough data, or crossfade disabled, flush the old data instead */
    if (LOW_DATA(2) || !pcmbuf_is_crossfade_enabled() || low_latency_mode)
    {
        pcmbuf_flush_fillpos();
        pcmbuf_flush = true;
        return false;
    }

    /* Don't enable mix mode when skipping tracks manually. */
    if (manual_skip)
        crossfade_mixmode = false;
    else
        crossfade_mixmode = global_settings.crossfade_fade_out_mixmode;

    crossfade_init = true;

    return true;

}

static void pcmbuf_finish_crossfade_enable(void)
{
    /* Copy the pending setting over now */
    crossfade_enabled = crossfade_enable_request;
    
    pcmbuf_watermark = (crossfade_enabled && pcmbuf_size) ?
        /* If crossfading, try to keep the buffer full other than 1 second */
        (pcmbuf_size - (NATIVE_FREQUENCY * 4 * 1)) :
        /* Otherwise, just use the default */
        PCMBUF_WATERMARK;
}

static bool pcmbuf_is_crossfade_enabled(void)
{
    if (global_settings.crossfade == CROSSFADE_ENABLE_SHUFFLE)
        return global_settings.playlist_shuffle;

    return crossfade_enabled;
}

bool pcmbuf_is_crossfade_active(void)
{
    return crossfade_active || crossfade_init;
}

void pcmbuf_request_crossfade_enable(bool on_off)
{
    /* Next setting to be used, not applied now */
    crossfade_enable_request = on_off;
}

bool pcmbuf_is_same_size(void)
{
    bool same_size;
    
    if (pcmbuffer == NULL)
        same_size = true;   /* Not set up yet even once so always */
    else
    {
        size_t bufsize = pcmbuf_get_next_required_pcmbuf_size();
        same_size = pcmbuf_calc_pcmbuffer_ptr(bufsize) == pcmbuffer;
    }
    
    if (same_size)
        pcmbuf_finish_crossfade_enable();
    
    return same_size;
}


/* Voice */

/* Returns pcm buffer usage in percents (0 to 100). */
static int pcmbuf_usage(void)
{
    return pcmbuf_unplayed_bytes * 100 / pcmbuf_size;
}

static int pcmbuf_mix_free(void)
{
    if (mix_chunk)
    {
        size_t my_mix_end =
            (size_t)&((int16_t *)mix_chunk->addr)[pcmbuf_mix_sample];
        size_t my_write_pos = (size_t)&pcmbuffer[pcmbuffer_pos];
        if (my_write_pos < my_mix_end)
            my_write_pos += pcmbuf_size;
        return (my_write_pos - my_mix_end) * 100 / pcmbuf_unplayed_bytes;
    }
    return 100;
}

void *pcmbuf_request_voice_buffer(int *count)
{
    /* A get-it-to-work-for-now hack (audio status could change by
       completion) */
    if (audio_status() & AUDIO_STATUS_PLAY)
    {
        if (read_chunk == NULL)
        {
            return NULL;
        }
        else if (pcmbuf_usage() >= 10 && pcmbuf_mix_free() >= 30 &&
                 (mix_chunk || read_chunk->link))
        {
            *count = MIN(*count, PCMBUF_MIX_CHUNK/4);
            return voicebuf;
        }
        else
        {
            return NULL;
        }
    }
    else
    {
        return pcmbuf_request_buffer(count);
    }
}

void pcmbuf_write_voice_complete(int count)
{
    /* A get-it-to-work-for-now hack (audio status could have changed) */
    if (!(audio_status() & AUDIO_STATUS_PLAY))
    {
        pcmbuf_write_complete(count);
        return;
    }

    int16_t *ibuf = (int16_t *)voicebuf;
    int16_t *obuf;
    size_t chunk_samples;

    if (mix_chunk == NULL && read_chunk != NULL)
    {
        mix_chunk = read_chunk->link;
        /* Start 1/8s into the next chunk */
        pcmbuf_mix_sample = NATIVE_FREQUENCY * 4 / 16;
    }

    if (!mix_chunk)
        return;

    obuf = (int16_t *)mix_chunk->addr;
    chunk_samples = mix_chunk->size / sizeof (int16_t);

    count <<= 1;

    while (count-- > 0)
    {
        int32_t sample = *ibuf++;

        if (pcmbuf_mix_sample >= chunk_samples)
        {
            mix_chunk = mix_chunk->link;
            if (!mix_chunk)
                return;
            pcmbuf_mix_sample = 0;
            obuf = mix_chunk->addr;
            chunk_samples = mix_chunk->size / 2;
        }
        sample += obuf[pcmbuf_mix_sample] >> 2;
        obuf[pcmbuf_mix_sample++] = clip_sample_16(sample);
    }
}


/* Debug menu, other metrics */

/* Amount of bytes left in the buffer. */
size_t pcmbuf_free(void)
{
    if (read_chunk != NULL)
    {
        void *read = read_chunk->addr;
        void *write = &pcmbuffer[pcmbuffer_pos + pcmbuffer_fillpos];
        if (read < write)
            return (size_t)(read - write) + pcmbuf_size;
        else
            return (size_t) (read - write);
    }
    return pcmbuf_size;
}

size_t pcmbuf_get_bufsize(void)
{
    return pcmbuf_size;
}

int pcmbuf_used_descs(void)
{
    struct chunkdesc *temp = read_chunk;
    unsigned int i = 0;
    while (temp) {
        temp = temp->link;
        i++;
    }
    return i;
}

int pcmbuf_descs(void)
{
    return CHUNK_DESCS(pcmbuf_size);
}

#ifdef ROCKBOX_HAS_LOGF
unsigned char * pcmbuf_get_meminfo(size_t *length)
{
    *length = pcmbuf_bufend - pcmbuffer;
    return pcmbuffer;
}
#endif


/* Misc */

bool pcmbuf_is_lowdata(void)
{
    if (!pcm_is_playing() || pcm_is_paused() ||
            crossfade_init || crossfade_active)
        return false;

#if MEMORYSIZE > 2
    /* 1 seconds of buffer is low data */
    return LOW_DATA(4);
#else
    /* under watermark is low data */
    return (pcmbuf_unplayed_bytes < pcmbuf_watermark);
#endif
}

void pcmbuf_set_low_latency(bool state)
{
    low_latency_mode = state;
}

unsigned long pcmbuf_get_latency(void)
{
    /* Be careful how this calculation is rearranged, it's easy to overflow */
    size_t bytes = pcmbuf_unplayed_bytes + pcm_get_bytes_waiting();
    return bytes / 4 * 1000 / NATIVE_FREQUENCY;
}

#ifndef HAVE_HARDWARE_BEEP
#define MINIBUF_SAMPLES (NATIVE_FREQUENCY / 1000 * KEYCLICK_DURATION)
#define MINIBUF_SIZE (MINIBUF_SAMPLES*4)

/* Generates a constant square wave sound with a given frequency
   in Hertz for a duration in milliseconds. */
void pcmbuf_beep(unsigned int frequency, size_t duration, int amplitude)
{
    unsigned int step = 0xffffffffu / NATIVE_FREQUENCY * frequency;
    int32_t phase = 0;
    int16_t *bufptr, *bufstart, *bufend;
    int32_t sample;
    int nsamples = NATIVE_FREQUENCY / 1000 * duration;
    bool mix = read_chunk != NULL && read_chunk->link != NULL;
    int i;

    bufend = SKIPBYTES((int16_t *)pcmbuffer, pcmbuf_size);

    /* Find the insertion point and set bufstart to the start of it */
    if (mix)
    {
        /* Get the currently playing chunk at the current position. */
        bufstart = (int16_t *)pcm_play_dma_get_peak_buffer(&i);

        /* If above isn't implemented or pcm is stopped, no beepeth. */
        if (!bufstart || !pcm_is_playing())
            return;

        /* Give 5ms clearance. */    
        bufstart += NATIVE_FREQUENCY * 4 / 200;

#ifdef HAVE_PCM_DMA_ADDRESS
        /* Returned peak addresses are DMA addresses */
        bufend = pcm_dma_addr(bufend);
#endif

        /* Wrapped above? */
        if (bufstart >= bufend)
            bufstart -= pcmbuf_size;

        /* NOTE: On some targets using hardware DMA, cache range flushing may
         * be required or the writes may not be picked up by the controller.
         * An incremental flush should be done periodically during the mixdown. */
    }
    else if (nsamples <= MINIBUF_SAMPLES)
    {
        static int16_t minibuf[MINIBUF_SAMPLES*2] __attribute__((aligned(4)));
        /* Use mini buffer */
        bufstart = minibuf;
        bufend = SKIPBYTES(bufstart, MINIBUF_SIZE);
    }
    else if (audio_buffer_state() != AUDIOBUF_STATE_TRASHED)
    {
        /* Use pcmbuffer */
        bufstart = (int16_t *)pcmbuffer;
    }
    else
    {
        /* No place */
        return;
    }

    bufptr = bufstart;

    /* Mix square wave into buffer */
    for (i = 0; i < nsamples; ++i)
    {
        int32_t amp = (phase >> 31) ^ (int32_t)amplitude;
        sample = mix ? *bufptr : 0;
        *bufptr++ = clip_sample_16(sample + amp);
        if (bufptr >= bufend)
            bufptr = (int16_t *)pcmbuffer;
        sample = mix ? *bufptr : 0;
        *bufptr++ = clip_sample_16(sample + amp);
        if (bufptr >= bufend)
            bufptr = (int16_t *)pcmbuffer;

        phase += step;
    }

    pcm_play_lock();
#ifdef HAVE_RECORDING
    pcm_rec_lock();
#endif

    /* Kick off playback if required and it won't interfere */
    if (!pcm_is_playing()
#ifdef HAVE_RECORDING
        && !pcm_is_recording()
#endif
        )
    {
        pcm_play_data(NULL, (unsigned char *)bufstart, nsamples * 4);
    }

    pcm_play_unlock();
#ifdef HAVE_RECORDING
    pcm_rec_unlock();
#endif
}
#endif /* HAVE_HARDWARE_BEEP */
