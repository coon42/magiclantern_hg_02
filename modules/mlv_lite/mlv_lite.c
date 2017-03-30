/**
 * RAW recording. Similar to lv_rec, with some different internals:
 * 
 * - buffering strategy:
 *      - group the frames in contiguous chunks, up to 32MB, to maximize writing speed
 *        (speed profile depends on buffer size: http://www.magiclantern.fm/forum/index.php?topic=5471 )
 *      - always write if there's something to write, even if that means using a small buffer
 *        (this minimizes idle time for the writing task, keeps memory free in the startup phase,
 *        and has no impact on the sustained write speeds
 *      - always choose the largest unused chunk => this maximizes the sustained writing speed 
 *        (small chunks will only be used in extreme situations, to squeeze the last few frames)
 *      - use any memory chunks that can contain at least one video frame
 *        (they will only be used when recording is about to stop, so no negative impact in sustained write speed)
 * 
 * - edmac_copy_rectangle: we can crop the image and trim the black borders!
 * - edmac operation done outside the LV task (in background, synchronized)
 * - on buffer overflow, it stops or skips frames (user-selected)
 * - using generic raw routines, no hardcoded stuff (should be easier to port)
 * - only for RAW in a single file (do one thing and do it well)
 * - goal #1: 1920x1080 on 1000x cards (achieved and exceeded, reports say 1920x1280 continuous!)
 * - goal #2: maximize number of frames for any given resolution + buffer + card speed configuration
 *   (see buffering strategy; I believe it's close to optimal, though I have no idea how to write a mathematical proof for it)
 */

/*
 * Copyright (C) 2013 Magic Lantern Team
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#define DEBUG_REDRAW_INTERVAL 1000   /* normally 1000; low values like 50 will reduce write speed a lot! */
#undef DEBUG_BUFFERING_GRAPH      /* some funky graphs */

#include <module.h>
#include <dryos.h>
#include <property.h>
#include <bmp.h>
#include <menu.h>
#include <config.h>
#include <math.h>
#include <cropmarks.h>
#include <screenshot.h>
#include "../lv_rec/lv_rec.h"
#include "edmac.h"
#include "edmac-memcpy.h"
#include "../file_man/file_man.h"
#include "cache_hacks.h"
#include "lvinfo.h"
#include "beep.h"
#include "raw.h"
#include "zebra.h"
#include "fps.h"
#include "../mlv_rec/mlv.h"
#include "../trace/trace.h"
#include "powersave.h"
#include "shoot.h"
#include "../silent/lossless.h"

/* from mlv_play module */
extern WEAK_FUNC(ret_0) void mlv_play_file(char *filename);

/* camera-specific tricks */
static int cam_eos_m = 0;
static int cam_5d2 = 0;
static int cam_50d = 0;
static int cam_500d = 0;
static int cam_550d = 0;
static int cam_6d = 0;
static int cam_600d = 0;
static int cam_650d = 0;
static int cam_7d = 0;
static int cam_700d = 0;
static int cam_60d = 0;

static int cam_5d3 = 0;
static int cam_5d3_113 = 0;
static int cam_5d3_123 = 0;

/**
 * resolution (in pixels) should be multiple of 16 horizontally (see http://www.magiclantern.fm/forum/index.php?topic=5839.0)
 * furthermore, resolution (in bytes) should be multiple of 8 in order to use the fastest EDMAC flags ( http://magiclantern.wikia.com/wiki/Register_Map#EDMAC ),
 * which copy 16 bytes at a time, but only check for overflows every 8 bytes (can be verified experimentally)
 * => if my math is not broken, this traslates to resolution being multiple of 32 pixels horizontally
 * use roughly 10% increments
 **/

static int resolution_presets_x[] = {  640,  960,  1280,  1600,  1920,  2240,  2560,  2880,  3200,  3520 };
#define  RESOLUTION_CHOICES_X CHOICES("640","960","1280","1600","1920","2240","2560","2880","3200","3520")

static int aspect_ratio_presets_num[]      = {   5,    4,    3,       8,      25,     239,     235,      22,    2,     185,     16,    5,    3,    4,    12,    1175,    1,    1 };
static int aspect_ratio_presets_den[]      = {   1,    1,    1,       3,      10,     100,     100,      10,    1,     100,      9,    3,    2,    3,    10,    1000,    1,    2 };
static const char * aspect_ratio_choices[] = {"5:1","4:1","3:1","2.67:1","2.50:1","2.39:1","2.35:1","2.20:1","2:1","1.85:1", "16:9","5:3","3:2","4:3","1.2:1","1.175:1","1:1","1:2"};

/* config variables */

CONFIG_INT("raw.video.enabled", raw_video_enabled, 0);

static CONFIG_INT("raw.res_x", resolution_index_x, 4);
static CONFIG_INT("raw.res_x_fine", res_x_fine, 0);
static CONFIG_INT("raw.aspect.ratio", aspect_ratio_index, 10);
static CONFIG_INT("raw.write.speed", measured_write_speed, 0);

static CONFIG_INT("raw.pre-record", pre_record, 0);
static int pre_record_triggered = 0;    /* becomes 1 once you press REC twice */
static int pre_record_num_frames = 0;   /* how many frames we should pre-record */

static CONFIG_INT("raw.dolly", dolly_mode, 0);
#define FRAMING_CENTER (dolly_mode == 0)
#define FRAMING_PANNING (dolly_mode == 1)

static CONFIG_INT("raw.preview", preview_mode, 0);
#define PREVIEW_AUTO (preview_mode == 0)
#define PREVIEW_CANON (preview_mode == 1)
#define PREVIEW_ML (preview_mode == 2)
#define PREVIEW_HACKED (preview_mode == 3)

static CONFIG_INT("raw.warm.up", warm_up, 0);
static CONFIG_INT("raw.use.srm.memory", use_srm_memory, 1);
static CONFIG_INT("raw.small.hacks", small_hacks, 1);

static CONFIG_INT("raw.h264.proxy", h264_proxy, 0);

static CONFIG_INT("raw.output_format", output_format, 3);
#define OUTPUT_14BIT_NATIVE 0
#define OUTPUT_12BIT_UNCOMPRESSED 1
#define OUTPUT_10BIT_UNCOMPRESSED 2
#define OUTPUT_14BIT_LOSSLESS 3
#define OUTPUT_12BIT_LOSSLESS 4
#define OUTPUT_10BIT_LOSSLESS 5
#define OUTPUT_COMPRESSION (output_format/3)

#define BPP (14 - 2*(output_format%3))

/* Recording Status Indicator Options */
#define INDICATOR_OFF        0
#define INDICATOR_IN_LVINFO  1
#define INDICATOR_ON_SCREEN  2
#define INDICATOR_RAW_BUFFER 3

static int show_graph = 0;

/* auto-choose the indicator style based on global draw settings */
/* GD off: only "on screen" works, obviously */
/* GD on: place it on the info bars to be minimally invasive */
#define indicator_display (show_graph ? INDICATOR_RAW_BUFFER : get_global_draw() ? INDICATOR_IN_LVINFO : INDICATOR_ON_SCREEN)

/* state variables */
static int res_x = 0;
static int res_y = 0;
static int max_res_x = 0;
static int max_res_y = 0;
static int sensor_res_x = 0;
static float squeeze_factor = 0;
static int max_frame_size = 0;
static int frame_size_uncompressed = 0;
static int skip_x = 0;
static int skip_y = 0;

static int frame_offset_x = 0;
static int frame_offset_y = 0;
static int frame_offset_delta_x = 0;
static int frame_offset_delta_y = 0;

#define RAW_IDLE      0
#define RAW_PREPARING 1
#define RAW_RECORDING 2
#define RAW_FINISHING 3
#define RAW_PRE_RECORDING 4

static int raw_recording_state = RAW_IDLE;
static int raw_previewing = 0;

#define RAW_IS_IDLE      (raw_recording_state == RAW_IDLE)
#define RAW_IS_PREPARING (raw_recording_state == RAW_PREPARING)
#define RAW_IS_RECORDING (raw_recording_state == RAW_RECORDING || \
                          raw_recording_state == RAW_PRE_RECORDING)
#define RAW_IS_FINISHING (raw_recording_state == RAW_FINISHING)

#define VIDF_HDR_SIZE 64

/* one video frame */
struct frame_slot
{
    void* ptr;          /* image data */
    int size;           /* total size, including overheads (VIDF, padding);
                           max_frame_size for uncompressed data, lower for compressed */
    int payload_size;   /* size effectively used by image data */
    int frame_number;   /* from 0 to n */
    enum {
        SLOT_FREE,          /* available for image capture */
        SLOT_RESERVED,      /* it may become available when resizing the previous slots */
        SLOT_FULL,          /* contains image data (can be fully captured or in progress - test with frame_check_saved) */
        SLOT_WRITING        /* it's being saved to card */
    } status;
};

static struct memSuite * shoot_mem_suite = 0;     /* memory suite for our buffers */
static struct memSuite * srm_mem_suite = 0;

static void * fullsize_buffers[2];                /* original image, before cropping, double-buffered */
static int fullsize_buffer_pos = 0;               /* which of the full size buffers (double buffering) is currently in use */
static int chunk_list[32];                        /* list of free memory chunk sizes, used for frame estimations */

static struct frame_slot slots[511];              /* frame slots */
static int slot_count = 0;                        /* how many frame slots we have */
static int capture_slot = -1;                     /* in what slot are we capturing now (index) */
static volatile int force_new_buffer = 0;         /* if some other task decides it's better to search for a new buffer */

static int writing_queue[COUNT(slots)+1];         /* queue of completed frames (slot indices) waiting to be saved */
static int writing_queue_tail = 0;                /* place captured frames here */
static int writing_queue_head = 0;                /* extract frames to be written from here */ 

static int frame_count = 0;                       /* how many frames we have processed */
static int chunk_frame_count = 0;                 /* how many frames in the current file chunk */
static int buffer_full = 0;                       /* true when the memory becomes full */
char* raw_movie_filename = 0;                     /* file name for current (or last) movie */
static char* chunk_filename = 0;                  /* file name for current movie chunk */
static int64_t written_total = 0;                 /* how many bytes we have written in this movie */
static int64_t written_chunk = 0;                 /* same for current chunk */
static int writing_time = 0;                      /* time spent by raw_video_rec_task in FIO_WriteFile calls */
static int idle_time = 0;                         /* time spent by raw_video_rec_task doing something else */
static uint32_t edmac_active = 0;

static mlv_file_hdr_t file_hdr;
static mlv_rawi_hdr_t rawi_hdr;
static mlv_idnt_hdr_t idnt_hdr;
static mlv_expo_hdr_t expo_hdr;
static mlv_lens_hdr_t lens_hdr;
static mlv_rtci_hdr_t rtci_hdr;
static mlv_wbal_hdr_t wbal_hdr;
static mlv_vidf_hdr_t vidf_hdr;
static uint64_t mlv_start_timestamp = 0;
uint32_t raw_rec_trace_ctx = TRACE_ERROR;

/* interface to other modules: these are called when recording starts or stops  */
extern WEAK_FUNC(ret_0) unsigned int raw_rec_cbr_starting();
extern WEAK_FUNC(ret_0) unsigned int raw_rec_cbr_stopping();

static int raw_rec_should_preview(void);

static void refresh_cropmarks()
{
    if (lv_dispsize > 1 || raw_rec_should_preview() || !raw_video_enabled)
    {
        reset_movie_cropmarks();
    }
    else
    {
        int x = RAW2BM_X(skip_x);
        int y = RAW2BM_Y(skip_y);
        int w = RAW2BM_DX(res_x);
        int h = RAW2BM_DY(res_y);
        
        set_movie_cropmarks(x, y, w, h);
    }
}

static int calc_res_y(int res_x, int max_res_y, int num, int den, float squeeze)
{
    int res_y;
    
    if (squeeze != 1.0f)
    {
        /* image should be enlarged vertically in post by a factor equal to "squeeze" */
        res_y = (int)(roundf(res_x * den / num / squeeze) + 1);
    }
    else
    {
        /* assume square pixels */
        res_y = (res_x * den / num + 1);
    }
    
    res_y = MIN(res_y, max_res_y);
    
    /* res_x * res_y must be modulo 16 bytes */
    switch (MOD(res_x * BPP / 8, 8))
    {
        case 0:     /* res_x is modulo 8 bytes, so res_y must be even */
            return res_y & ~1;

        case 4:     /* res_x is modulo 4 bytes, so res_y must be modulo 4 as well */
            return res_y & ~3;
        
        case 2:
        case 6:     /* res_x is modulo 2 bytes, so res_y must be modulo 8 */
            return res_y & ~7;

        default:    /* should be unreachable */
            return res_y & ~15;
    }
}

static void update_cropping_offsets()
{
    int sx = raw_info.active_area.x1 + (max_res_x - res_x) / 2;
    int sy = raw_info.active_area.y1 + (max_res_y - res_y) / 2;

    if (FRAMING_PANNING)
    {
        sx += frame_offset_x;
        sy += frame_offset_y;
    }
    else if (FRAMING_CENTER && lv_dispsize > 1)
    {
        /* try to center the recording window on the YUV frame */
        int delta_x, delta_y;
        int ok = focus_box_get_raw_crop_offset(&delta_x, &delta_y);
        if (ok)
        {
            sx = COERCE(sx - delta_x, raw_info.active_area.x1, raw_info.active_area.x2 - res_x);
            sy = COERCE(sy - delta_y, raw_info.active_area.y1, raw_info.active_area.y2 - res_y);
        }
    }

    skip_x = sx;
    skip_y = sy;
    
    refresh_cropmarks();
    
    /* mv640crop needs this to center the recorded image */
    if (is_movie_mode() && video_mode_resolution == 2 && video_mode_crop)
    {
        skip_x = skip_x + 51;
        skip_y = skip_y - 6;
    }
}

static void update_resolution_params()
{
    /* max res X */
    /* make sure we don't get dead pixels from rounding */
    int left_margin = (raw_info.active_area.x1 + 7) / 8 * 8;
    int right_margin = (raw_info.active_area.x2) / 8 * 8;
    int max = (right_margin - left_margin);
    
    /* max image width is modulo 2 bytes and 8 pixels */
    /* (EDMAC requires W x H to be modulo 16 bytes) */
    /* (processing tools require W modulo 8 pixels for struct raw_pixblock) */
    
    max_res_x = max;
    
    /* max res Y */
    max_res_y = raw_info.jpeg.height & ~1;

    /* squeeze factor */
    if (video_mode_resolution == 1 && lv_dispsize == 1 && is_movie_mode()) /* 720p, image squeezed */
    {
        /* 720p mode uses 5x3 binning (5DMK3)
         * or 5x3 horizontal binning + vertical skipping (other cameras) */
        squeeze_factor = 5.0 / 3.0;
    }
    else squeeze_factor = 1.0f;

    /* res X */
    res_x = MIN(resolution_presets_x[resolution_index_x] + res_x_fine, max_res_x);

    /* res Y */
    int num = aspect_ratio_presets_num[aspect_ratio_index];
    int den = aspect_ratio_presets_den[aspect_ratio_index];
    res_y = calc_res_y(res_x, max_res_y, num, den, squeeze_factor);

    /* check EDMAC restrictions (W * H multiple of 16 bytes) */
    ASSERT((res_x * BPP / 8 * res_y) % 16 == 0);

    /* frame size */
    /* should be multiple of 512, so there's no write speed penalty (see http://chdk.setepontos.com/index.php?topic=9970 ; confirmed by benchmarks) */
    /* let's try 64 for EDMAC alignment */
    /* 64 at the front for the VIDF header */
    /* 4 bytes after for checking EDMAC operation */
    int frame_size_padded = (VIDF_HDR_SIZE + (res_x * res_y * BPP/8) + 4 + 511) & ~511;
    
    /* frame size without padding */
    /* must be multiple of 4 */
    frame_size_uncompressed = res_x * res_y * BPP/8;
    ASSERT(frame_size_uncompressed % 4 == 0);
    
    max_frame_size = frame_size_padded;
    
    update_cropping_offsets();
}

static char* guess_aspect_ratio(int res_x, int res_y)
{
    static char msg[20];
    int best_num = 0;
    int best_den = 0;
    float ratio = (float)res_x / res_y;
    float minerr = 100;
    /* common ratios that are expressed as integer numbers, e.g. 3:2, 16:9, but not 2.35:1 */
    static int common_ratios_x[] = {1, 2, 3, 3, 4, 16, 5, 5};
    static int common_ratios_y[] = {1, 1, 1, 2, 3, 9,  4, 3};
    for (int i = 0; i < COUNT(common_ratios_x); i++)
    {
        int num = common_ratios_x[i];
        int den = common_ratios_y[i];
        float err = ABS((float)num / den - ratio);
        if (err < minerr)
        {
            minerr = err;
            best_num = num;
            best_den = den;
        }
    }
    
    if (minerr < 0.05)
    {
        int h = calc_res_y(res_x, max_res_y, best_num, best_den, squeeze_factor);
        /* if the difference is 1 pixel, consider it exact */
        char* qualifier = ABS(h - res_y) > 1 ? "almost " : "";
        snprintf(msg, sizeof(msg), "%s%d:%d", qualifier, best_num, best_den);
    }
    else if (ratio > 1)
    {
        int r = (int)roundf(ratio * 100);
        /* is it 2.35:1 or 2.353:1? */
        int h = calc_res_y(res_x, max_res_y, r, 100, squeeze_factor);
        char* qualifier = ABS(h - res_y) > 1 ? "almost " : "";
        if (r%100) snprintf(msg, sizeof(msg), "%s%d.%02d:1", qualifier, r/100, r%100);
    }
    else
    {
        int r = (int)roundf((1/ratio) * 100);
        int h = calc_res_y(res_x, max_res_y, 100, r, squeeze_factor);
        char* qualifier = ABS(h - res_y) > 1 ? "almost " : "";
        if (r%100) snprintf(msg, sizeof(msg), "%s1:%d.%02d", qualifier, r/100, r%100);
    }
    return msg;
}

static int predict_frames(int write_speed)
{
    int fps = fps_get_current_x1000();
    int capture_speed = max_frame_size / 1000 * fps;
    int buffer_fill_speed = capture_speed - write_speed;
    if (buffer_fill_speed <= 0)
        return INT_MAX;
    
    int total_slots = 0;
    for (int i = 0; i < COUNT(chunk_list); i++)
        total_slots += chunk_list[i] / max_frame_size;
    
    float buffer_fill_time = total_slots * max_frame_size / (float) buffer_fill_speed;
    int frames = buffer_fill_time * fps / 1000;
    return frames;
}

/* how many frames can we record with current settings, without dropping? */
static char* guess_how_many_frames()
{
    if (!measured_write_speed) return "";
    if (!chunk_list[0]) return "";
    
    int write_speed_lo = measured_write_speed * 1024 / 100 * 1024 - 512 * 1024;
    int write_speed_hi = measured_write_speed * 1024 / 100 * 1024 + 512 * 1024;
    
    int f_lo = predict_frames(write_speed_lo);
    int f_hi = predict_frames(write_speed_hi);
    
    static char msg[50];
    if (f_lo < 5000)
    {
        int write_speed = (write_speed_lo + write_speed_hi) / 2;
        write_speed = (write_speed * 10 + 512 * 1024) / (1024 * 1024);
        if (f_lo != f_hi)
            snprintf(msg, sizeof(msg), "Expect %d-%d frames at %d.%dMB/s.", f_lo, f_hi, write_speed / 10, write_speed % 10);
        else
            snprintf(msg, sizeof(msg), "Expect around %d frames at %d.%dMB/s.", f_lo, write_speed / 10, write_speed % 10);
    }
    else
    {
        snprintf(msg, sizeof(msg), "Continuous recording OK.");
    }
    
    return msg;
}

static MENU_UPDATE_FUNC(write_speed_update)
{
    int fps = fps_get_current_x1000();
    int speed = (res_x * res_y * BPP/8 / 1024) * fps / 10 / 1024;
    int ok = speed < measured_write_speed;
    speed /= 10;

    if (max_frame_size % 512)
    {
        MENU_SET_WARNING(MENU_WARN_ADVICE, "Frame size not multiple of 512 bytes!");
    }
    else
    {
        if (!measured_write_speed)
            MENU_SET_WARNING(ok ? MENU_WARN_INFO : MENU_WARN_ADVICE, 
                "Write speed needed: %d.%d MB/s at %d.%03d fps.",
                speed/10, speed%10, fps/1000, fps%1000
            );
        else
            MENU_SET_WARNING(ok ? MENU_WARN_INFO : MENU_WARN_ADVICE, 
                "%d.%d MB/s at %d.%03dp. %s",
                speed/10, speed%10, fps/1000, fps%1000,
                guess_how_many_frames()
            );
    }
}

static void refresh_raw_settings(int force)
{
    if (!lv) return;
    
    if (RAW_IS_IDLE && !raw_previewing)
    {
        /* autodetect the resolution (update 4 times per second) */
        static int aux = INT_MIN;
        if (force || should_run_polling_action(250, &aux))
        {
            if (raw_update_params())
            {
                update_resolution_params();
            }
        }
    }
}

PROP_HANDLER( PROP_LV_AFFRAME ) {
    ASSERT(len <= 128);
    if(!lv) return;
    
    sensor_res_x = ((int32_t*)buf)[0];
}


static int32_t calc_crop_factor()
{

    int32_t camera_crop = 162;
    int32_t sampling_x = 3;
    
    if (cam_5d2 || cam_5d3 || cam_6d) camera_crop = 100;
    
    //if (cam_500d || cam_50d) sensor_res_x = 4752;
    //if (cam_eos_m || cam_550d || cam_600d || cam_650d || cam_700d || cam_60d || cam_7d) sensor_res_x = 5184;
    //if (cam_6d) sensor_res_x = 5472;
    //if (cam_5d2) sensor_res_x = 5616;
    //if (cam_5d3) sensor_res_x = 5760;
    
    if (video_mode_crop || (lv_dispsize > 1)) sampling_x = 1;
    
    if (!sensor_res_x) return 0;
    
    return camera_crop * (sensor_res_x / sampling_x) / res_x;
}

static MENU_UPDATE_FUNC(raw_main_update)
{
    // reset_movie_cropmarks if raw_rec is disabled
    refresh_cropmarks();
    
    if (!raw_video_enabled) return;
    
    refresh_raw_settings(0);

    if (!RAW_IS_IDLE)
    {
        MENU_SET_VALUE(RAW_IS_RECORDING ? "Recording..." : RAW_IS_PREPARING ? "Starting..." : RAW_IS_FINISHING ? "Stopping..." : "err");
        MENU_SET_ICON(MNI_RECORD, 0);
    }
    else
    {
        MENU_SET_VALUE("ON, %dx%d", res_x, res_y);
        int32_t crop_factor = calc_crop_factor();
        if (crop_factor) MENU_SET_RINFO("%s%d.%02dx", FMT_FIXEDPOINT2( crop_factor ));
    }

    write_speed_update(entry, info);
}

static MENU_UPDATE_FUNC(aspect_ratio_update_info)
{
    if (squeeze_factor == 1.0f)
    {
        char* ratio = guess_aspect_ratio(res_x, res_y);
        MENU_SET_HELP("%dx%d (%s).", res_x, res_y, ratio);
    }
    else
    {
        int num = aspect_ratio_presets_num[aspect_ratio_index];
        int den = aspect_ratio_presets_den[aspect_ratio_index];
        int sq100 = (int)roundf(squeeze_factor*100);
        int res_y_corrected = calc_res_y(res_x, max_res_y, num, den, 1.0f);
        MENU_SET_HELP("%dx%d. Stretch by %s%d.%02dx to get %dx%d (%s) in post.", res_x, res_y, FMT_FIXEDPOINT2(sq100), res_x, res_y_corrected, aspect_ratio_choices[aspect_ratio_index]);
    }
}

static MENU_UPDATE_FUNC(resolution_update)
{
    if (!raw_video_enabled || !lv)
    {
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Enable RAW video first.");
        MENU_SET_VALUE("N/A");
        return;
    }
    
    res_x = resolution_presets_x[resolution_index_x] + res_x_fine;
       
    refresh_raw_settings(1);

    int selected_x = res_x;

    MENU_SET_VALUE("%dx%d", res_x, res_y);
    int crop_factor = calc_crop_factor();
    if (crop_factor) MENU_SET_RINFO("%s%d.%02dx", FMT_FIXEDPOINT2( crop_factor ));
    
    if (selected_x > max_res_x)
    {
        MENU_SET_HELP("%d is not possible in current video mode (max %d).", selected_x, max_res_x);
    }
    else
    {
        aspect_ratio_update_info(entry, info);
    }

    write_speed_update(entry, info);
    
    if (!get_menu_edit_mode())
    {
        int len = strlen(info->help);
        if (len < 20)
        {
            snprintf(info->help + len, MENU_MAX_HELP_LEN - len,
                " Fine-tune with LEFT/RIGHT or top scrollwheel."
            );
        }
    }
}

static MENU_SELECT_FUNC(resolution_change_fine_value)
{
    if (!raw_video_enabled || !lv)
    {
        return;
    }
    
    if (get_menu_edit_mode()) {
        /* pickbox: select a preset */
        resolution_index_x = COERCE(resolution_index_x + delta, 0, COUNT(resolution_presets_x) - 1);
        res_x_fine = 0;
        return;
    }
    
    /* fine-tune resolution in small increments */
    int cur_res = ((resolution_presets_x[resolution_index_x] + res_x_fine) + 15) & ~15;
    cur_res = COERCE(cur_res + delta * 16, resolution_presets_x[0], max_res_x); 

    /* select the closest preset */
    int max_delta = INT_MAX;
    for (int i = 0; i < COUNT(resolution_presets_x); i++)
    {
        int preset_res = resolution_presets_x[i];
        int delta = MAX(cur_res * 1024 / preset_res, preset_res * 1024 / cur_res);
        if (delta < max_delta)
        {
            resolution_index_x = i;
            max_delta = delta;
        }
    }
    res_x_fine = cur_res - resolution_presets_x[resolution_index_x];
}

static MENU_UPDATE_FUNC(aspect_ratio_update)
{
    if (!raw_video_enabled || !lv)
    {
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Enable RAW video first.");
        MENU_SET_VALUE("N/A");
        return;
    }
    
    refresh_raw_settings(0);

    int num = aspect_ratio_presets_num[aspect_ratio_index];
    int den = aspect_ratio_presets_den[aspect_ratio_index];
    int selected_y = calc_res_y(res_x, max_res_y, num, den, squeeze_factor);
    
    if (selected_y > max_res_y + 2)
    {
        char* ratio = guess_aspect_ratio(res_x, res_y * squeeze_factor);
        MENU_SET_VALUE(ratio);
        MENU_SET_HELP("Could not get %s. Max vertical resolution: %d.", aspect_ratio_choices[aspect_ratio_index], res_y);
    }
    else
    {
        aspect_ratio_update_info(entry, info);
    }
    write_speed_update(entry, info);
}

static void add_reserved_slots(void * ptr, int n)
{
    /* each group has some additional (empty) slots,
     * to be used when frames are compressed
     * (we don't know the compressed size in advance,
     * so we'll resize them on the fly) */
    for (int i = 0; i < n; i++)
    {
        slots[slot_count].ptr = ptr;
        slots[slot_count].size = 0;
        slots[slot_count].status = SLOT_RESERVED;
        slot_count++;
    }
}

static int add_mem_suite(struct memSuite * mem_suite, int chunk_index)
{
    if(mem_suite)
    {
        /* use all chunks larger than max_frame_size for recording */
        struct memChunk * chunk = GetFirstChunkFromSuite(mem_suite);
        while(chunk)
        {
            int size = GetSizeOfMemoryChunk(chunk);
            intptr_t ptr = (intptr_t) GetMemoryAddressOfMemoryChunk(chunk);

            /* write it down for future frame predictions */
            if (chunk_index < COUNT(chunk_list) && size > 64)
            {
                chunk_list[chunk_index] = size - 64;
                printf("chunk #%d: size=%x (%x)\n",
                    chunk_index+1, chunk_list[chunk_index],
                    format_memory_size(chunk_list[chunk_index])
                );
                chunk_index++;
            }
            
            /* align pointer at 64 bytes */
            intptr_t ptr_raw = ptr;
            ptr   = (ptr + 63) & ~63;
            size -= (ptr - ptr_raw);

            /* fit as many frames as we can */
            int group_size = 0;
            while (size >= max_frame_size && slot_count < COUNT(slots))
            {
                slots[slot_count].ptr = (void*) ptr;
                slots[slot_count].size = max_frame_size;
                slots[slot_count].payload_size = frame_size_uncompressed;
                slots[slot_count].status = SLOT_FREE;
                ptr += max_frame_size;
                size -= max_frame_size;
                group_size += max_frame_size;
                slot_count++;
                //printf("slot #%d: %x\n", slot_count, ptr);

                /* split the group at 32M-512K */
                /* (after this number, write speed decreases) */
                /* (CFDMA can write up to FFFF sectors at once) */
                /* (FFFE just in case) */
                if (group_size + max_frame_size > 0xFFFE * 512)
                {
                    /* insert a small gap to split the group here */
                    add_reserved_slots((void*)ptr, group_size / max_frame_size);
                    ptr += 64;
                    size -= 64;
                    group_size = 0;
                }
            }
            
            add_reserved_slots((void*)ptr, group_size / max_frame_size);
            
            /* next chunk */
            chunk = GetNextMemoryChunk(mem_suite, chunk);
        }
    }
    
    return chunk_index;
}

static int setup_buffers()
{
    /* allocate memory for double buffering */
    /* (we need a single large contiguous chunk) */
    int buf_size = raw_info.width * raw_info.height * BPP/8 * 33/32; /* leave some margin, just in case */
    ASSERT(fullsize_buffers[0] == 0);
    fullsize_buffers[0] = fio_malloc(buf_size);
    
    /* reuse Canon's buffer */
    fullsize_buffers[1] = UNCACHEABLE(raw_info.buffer);

    /* anything wrong? */
    if(fullsize_buffers[0] == 0 || fullsize_buffers[1] == 0)
    {
        /* buffers will be freed by caller in the cleanup section */
        return 0;
    }

    /* allocate the entire memory, but only use large chunks */
    /* yes, this may be a bit wasteful, but at least it works */
    
    memset(chunk_list, 0, sizeof(chunk_list));
    
    shoot_mem_suite = shoot_malloc_suite(0);
    srm_mem_suite = use_srm_memory ? srm_malloc_suite(0) : 0;
    
    if (!shoot_mem_suite && !srm_mem_suite)
    {
        return 0;
    }
        
    int chunk_index = 0;
    chunk_index = add_mem_suite(shoot_mem_suite, chunk_index);
    chunk_index = add_mem_suite(srm_mem_suite, chunk_index);
  
    /* we need at least 3 slots */
    if (slot_count < 3)
    {
        return 0;
    }
    
    if (pre_record)
    {
        /* how much should we pre-record? */
        const int presets[4] = {1, 2, 5, 10};
        int requested_seconds = presets[(pre_record-1) & 3];
        int requested_frames = requested_seconds * fps_get_current_x1000() / 1000;

        /* leave at least 16MB for buffering */
        int max_frames = slot_count - 16*1024*1024 / max_frame_size;
        pre_record_num_frames = MIN(requested_frames, max_frames);
    }
    
    return 1;
}

static void free_buffers()
{
    if (shoot_mem_suite) shoot_free_suite(shoot_mem_suite);
    shoot_mem_suite = 0;
    if (srm_mem_suite) srm_free_suite(srm_mem_suite);
    srm_mem_suite = 0;
    if (fullsize_buffers[0]) fio_free(fullsize_buffers[0]);
    fullsize_buffers[0] = 0;
}

static int get_free_slots()
{
    int free_slots = 0;
    for (int i = 0; i < slot_count; i++)
        if (slots[i].status == SLOT_FREE)
            free_slots++;
    return free_slots;
}

#define BUFFER_DISPLAY_X 30
#define BUFFER_DISPLAY_Y 50

static void show_buffer_status()
{
    if (!liveview_display_idle()) return;
    
    int scale = MAX(1, (300 / slot_count + 1) & ~1);
    int x = BUFFER_DISPLAY_X;
    int y = BUFFER_DISPLAY_Y;
    for (int i = 0; i < slot_count; i++)
    {
        if (i > 0 && slots[i].ptr != slots[i-1].ptr + slots[i-1].size)
            x += MAX(2, scale);

        int color = slots[i].status == SLOT_FREE    ? COLOR_BLACK :
                    slots[i].status == SLOT_WRITING ? COLOR_GREEN1 :
                    slots[i].status == SLOT_FULL    ? COLOR_LIGHT_BLUE :
                                                      COLOR_RED ;
        for (int k = 0; k < scale; k++)
        {
            draw_line(x, y+5, x, y+17, color);
            x++;
        }
        
        if (scale > 3)
            x++;
    }

#ifdef DEBUG_BUFFERING_GRAPH
    {
        int free = get_free_slots();
        int x = frame_count % 720;
        int ymin = 120;
        int ymax = 400;
        int y = ymin + free * (ymax - ymin) / slot_count;
        dot(x-16, y-16, COLOR_BLACK, 3);
        static int prev_x = 0;
        static int prev_y = 0;
        if (prev_x && prev_y && prev_x < x)
        {
            draw_line(prev_x, prev_y, x, y, COLOR_BLACK);
        }
        prev_x = x;
        prev_y = y;
        bmp_draw_rect(COLOR_BLACK, 0, ymin, 720, ymax-ymin);
        
        int xp = predict_frames(measured_write_speed * 1024 / 100 * 1024) % 720;
        draw_line(xp, ymax, xp, ymin, COLOR_RED);
    }
#endif
}

static void panning_update()
{
    if (!FRAMING_PANNING) return;

    int sx = raw_info.active_area.x1 + (max_res_x - res_x) / 2;
    int sy = raw_info.active_area.y1 + (max_res_y - res_y) / 2;

    frame_offset_x = COERCE(
        frame_offset_x + frame_offset_delta_x, 
        raw_info.active_area.x1 - sx,
        raw_info.active_area.x1 + max_res_x - res_x - sx
    );
    
    frame_offset_y = COERCE(
        frame_offset_y + frame_offset_delta_y, 
        raw_info.active_area.y1 - sy,
        raw_info.active_area.y1 + max_res_y - res_y - sy
    );

    update_cropping_offsets();
}

static void raw_video_enable()
{
    /* toggle the lv_save_raw flag from raw.c */
    raw_lv_request();
    
    msleep(50);
}

static void raw_video_disable()
{
    raw_lv_release();
}

static void raw_lv_request_update()
{
    static int raw_lv_requested = 0;

    if (raw_video_enabled && lv && is_movie_mode())
    {
        if (!raw_lv_requested)
        {
            raw_video_enable();
            raw_lv_requested = 1;
        }
    }
    else
    {
        if (raw_lv_requested)
        {
            raw_video_disable();
            raw_lv_requested = 0;
        }
    }
}

/* Display recording status in top info bar */
static LVINFO_UPDATE_FUNC(recording_status)
{
    LVINFO_BUFFER(16);
    
    if ((indicator_display != INDICATOR_IN_LVINFO) || RAW_IS_IDLE) return;

    /* Calculate the stats */
    int fps = fps_get_current_x1000();
    int t = (frame_count * 1000 + fps/2) / fps;
    int predicted = predict_frames(measured_write_speed * 1024 / 100 * 1024);

    if (!buffer_full) 
    {
        snprintf(buffer, sizeof(buffer), "%02d:%02d", t/60, t%60);
        if (raw_recording_state == RAW_PRE_RECORDING)
        {
            item->color_bg = COLOR_BLUE;
        }
        else if (predicted >= 10000)
        {
            item->color_bg = COLOR_GREEN1;
        }
        else
        {
            int time_left = (predicted-frame_count) * 1000 / fps;
            if (time_left < 10) {
                item->color_bg = COLOR_DARK_RED;
            } else {
                item->color_bg = COLOR_YELLOW;
            }
        }
    } 
    else 
    {
        snprintf(buffer, sizeof(buffer), "Stopped.");
        item->color_bg = COLOR_DARK_RED;
    }
}

/* Display the 'Recording...' icon and status */
static void show_recording_status()
{
    /* Determine if we should redraw */
    static int auxrec = INT_MIN;
    if (RAW_IS_RECORDING && liveview_display_idle() && should_run_polling_action(DEBUG_REDRAW_INTERVAL, &auxrec))
    {
        /* Calculate the stats */
        int fps = fps_get_current_x1000();
        int t = (frame_count * 1000 + fps/2) / fps;
        int predicted = predict_frames(measured_write_speed * 1024 / 100 * 1024);

        int speed=0;
        int idle_percent=0;
        if (writing_time)
        {
            speed = (int)((float)written_total / (float)writing_time * (1000.0f / 1024.0f / 1024.0f * 100.0f)); // KiB and msec -> MiB/s x100
            idle_percent = idle_time * 100 / (writing_time + idle_time);
            measured_write_speed = speed;
            speed /= 10;
        }

        if (indicator_display == INDICATOR_IN_LVINFO)
        {
            /* If displaying in the info bar, force a refresh */
            lens_display_set_dirty();
        }
        else if (indicator_display == INDICATOR_RAW_BUFFER)
        {
            show_buffer_status();

            if (predicted < 10000)
                bmp_printf( FONT(FONT_MED, COLOR_WHITE, COLOR_BG_DARK), BUFFER_DISPLAY_X, BUFFER_DISPLAY_Y+22,
                    "%02d:%02d, %d frames / %d expected  ",
                    t/60, t%60,
                    frame_count,
                    predicted
                );
            else
                bmp_printf( FONT(FONT_MED, COLOR_WHITE, COLOR_BG_DARK), BUFFER_DISPLAY_X, BUFFER_DISPLAY_Y+22,
                    "%02d:%02d, %d frames, continuous OK  ",
                    t/60, t%60,
                    frame_count
                );

            if (writing_time)
            {
                char msg[50];
                snprintf(msg, sizeof(msg),
                    "%s: %d MB, %d.%d MB/s",
                    chunk_filename + 17, /* skip A:/DCIM/100CANON/ */
                    (int)(written_total / 1024 / 1024),
                    speed/10, speed%10
                );
                if (idle_time)
                {
                    if (idle_percent) { STR_APPEND(msg, ", %d%% idle", idle_percent); }
                    else { STR_APPEND(msg, ", %dms idle", idle_time); }
                }
                bmp_printf( FONT(FONT_MED, COLOR_WHITE, COLOR_BG_DARK), BUFFER_DISPLAY_X, BUFFER_DISPLAY_Y+22+font_med.height, "%s", msg);
            }
        }
        else if (indicator_display == INDICATOR_ON_SCREEN)
        {

            /* Position the Recording Icon */
            int rl_x = 500;
            int rl_y = 40;

            /* If continuous OK, make the movie icon green, else set based on expected time left */
            int rl_color;
            if (raw_recording_state == RAW_PRE_RECORDING)
            {
                rl_color = COLOR_BLUE;
            }
            else if (predicted >= 10000) 
            {
                rl_color = COLOR_GREEN1;
            } 
            else 
            {
                int time_left = (predicted-frame_count) * 1000 / fps;
                if (time_left < 10) {
                    rl_color = COLOR_DARK_RED;
                } else {
                    rl_color = COLOR_YELLOW;
                }
            }

            int rl_icon_width=0;

            /* Draw the movie camera icon */
            rl_icon_width = bfnt_draw_char (ICON_ML_MOVIE,rl_x,rl_y,rl_color,COLOR_BG_DARK);

            /* Display the Status */
            bmp_printf (FONT(FONT_MED, COLOR_WHITE, COLOR_BG_DARK), rl_x+rl_icon_width+5, rl_y+5, "%02d:%02d", t/60, t%60);

            if (writing_time)
            {
                char msg[50];
                snprintf(msg, sizeof(msg), "%d.%01dMB/s", speed/10, speed%10);
                if (idle_time)
                {
                    if (idle_percent) { STR_APPEND(msg, ", %d%% idle  ", idle_percent); }
                    else { STR_APPEND(msg,", %dms idle  ", idle_time); }
                }
                bmp_printf (FONT(FONT_SMALL, COLOR_WHITE, COLOR_BG_DARK), rl_x+rl_icon_width+5, rl_y+5+font_med.height, "%s", msg);
            }
        }
    }
    return;
}

static unsigned int raw_rec_polling_cbr(unsigned int unused)
{
    raw_lv_request_update();
    
    if (!raw_video_enabled)
        return 0;
    
    if (!lv || !is_movie_mode())
        return 0;

    /* update settings when changing video modes (outside menu) */
    if (RAW_IS_IDLE && !gui_menu_shown())
    {
        refresh_raw_settings(0);
    }
    
    /* update status messages */
    show_recording_status();

    return 0;
}


/* todo: reference counting, like with raw_lv_request */
static void cache_require(int lock)
{
    static int cache_was_unlocked = 0;
    if (lock)
    {
        if (!cache_locked())
        {
            cache_was_unlocked = 1;
            icache_lock();
        }
    }
    else
    {
        if (cache_was_unlocked)
        {
            icache_unlock();
            cache_was_unlocked = 0;
        }
    }
}

static void unhack_liveview_vsync(int unused);

static void hack_liveview_vsync()
{
    if (cam_5d2 || cam_50d)
    {
        /* try to fix pink preview in zoom mode (5D2/50D) */
        if (lv_dispsize > 1 && !get_halfshutter_pressed())
        {
            if (RAW_IS_IDLE)
            {
                /**
                 * This register seems to be raw type on digic 4; digic 5 has it at c0f37014
                 * - default is 5 on 5D2 with lv_save_raw, 0xB without, 4 is lv_af_raw
                 * - don't record this: you will have lots of bad pixels (no big deal if you can remove them)
                 * - don't record lv_af_raw: you will have random colored dots that contain focus info; their position is not fixed, so you can't remove them
                 * - use half-shutter heuristic for clean silent pics
                 * 
                 * Reason for overriding here:
                 * - if you use lv_af_raw, you can no longer restore it when you start recording.
                 * - if you override here, image quality is restored as soon as you stop overriding
                 * - but pink preview is also restored, you can't have both
                 */
                
                *(volatile uint32_t*)0xc0f08114 = 0;
            }
            else
            {
                /**
                 * While recording, we will have pink image
                 * Make it grayscale and bring the shadows down a bit
                 * (these registers will only touch the preview, not the recorded image)
                 */
                *(volatile uint32_t*)0xc0f0f070 = 0x01000100;
                //~ *(volatile uint32_t*)0xc0f0e094 = 0;
                *(volatile uint32_t*)0xc0f0f1c4 = 0xFFFFFFFF;
            }
        }
    }
    
    if (!PREVIEW_HACKED) return;
    
    int rec = RAW_IS_RECORDING;
    static int prev_rec = 0;
    int should_hack = 0;
    int should_unhack = 0;

    if (rec)
    {
        if (frame_count == 0)
            should_hack = 1;
    }
    else if (prev_rec)
    {
        should_unhack = 1;
    }
    prev_rec = rec;
    
    if (should_hack)
    {
        int y = 100;
        for (int channel = 0; channel < 32; channel++)
        {
            /* silence out the EDMACs used for HD and LV buffers */
            int pitch = edmac_get_length(channel) & 0xFFFF;
            if (pitch == vram_lv.pitch || pitch == vram_hd.pitch)
            {
                uint32_t reg = edmac_get_base(channel);
                bmp_printf(FONT_SMALL, 30, y += font_small.height, "Hack %x %dx%d ", reg, shamem_read(reg + 0x10) & 0xFFFF, shamem_read(reg + 0x10) >> 16);
                *(volatile uint32_t *)(reg + 0x10) = shamem_read(reg + 0x10) & 0xFFFF;
            }
        }
    }
    else if (should_unhack)
    {
        task_create("lv_unhack", 0x1e, 0x1000, unhack_liveview_vsync, (void*)0);
    }
}

/* this is a separate task */
static void unhack_liveview_vsync(int unused)
{
    while (!RAW_IS_IDLE) msleep(100);
    PauseLiveView();
    ResumeLiveView();
}

static void hack_liveview(int unhack)
{
    if (small_hacks)
    {
        /* disable canon graphics (gains a little speed) */
        static int canon_gui_was_enabled;
        if (!unhack)
        {
            canon_gui_was_enabled = !canon_gui_front_buffer_disabled();
            canon_gui_disable_front_buffer();
        }
        else if (canon_gui_was_enabled)
        {
            canon_gui_enable_front_buffer(0);
            canon_gui_was_enabled = 0;
        }

        /* disable auto exposure and auto white balance */
        call("aewb_enableaewb", unhack ? 1 : 0);  /* for new cameras */
        call("lv_ae",           unhack ? 1 : 0);  /* for old cameras */
        call("lv_wb",           unhack ? 1 : 0);
        
        /* change dialog refresh timer from 50ms to 8192ms */
        uint32_t dialog_refresh_timer_addr = /* in StartDialogRefreshTimer */
            cam_50d ? 0xffa84e00 :
            cam_5d2 ? 0xffaac640 :
            cam_5d3_113 ? 0xff4acda4 :
            cam_5d3_123 ? 0xFF4B7648 :
            cam_550d ? 0xFF2FE5E4 :
            cam_600d ? 0xFF37AA18 :
            cam_650d ? 0xFF527E38 :
            cam_6d   ? 0xFF52C684 :
            cam_eos_m ? 0xFF539C1C :
            cam_700d ? 0xFF52BB60 :
            cam_7d  ? 0xFF345788 :
            cam_60d ? 0xff36fa3c :
            cam_500d ? 0xFF2ABEF8 :
            /* ... */
            0;
        uint32_t dialog_refresh_timer_orig_instr = 0xe3a00032; /* mov r0, #50 */
        uint32_t dialog_refresh_timer_new_instr  = 0xe3a00a02; /* change to mov r0, #8192 */

        if (*(volatile uint32_t*)dialog_refresh_timer_addr != dialog_refresh_timer_orig_instr)
        {
            /* something's wrong */
            NotifyBox(1000, "Hack error at %x:\nexpected %x, got %x", dialog_refresh_timer_addr, dialog_refresh_timer_orig_instr, *(volatile uint32_t*)dialog_refresh_timer_addr);
            beep_custom(1000, 2000, 1);
            dialog_refresh_timer_addr = 0;
        }

        if (dialog_refresh_timer_addr)
        {
            if (!unhack) /* hack */
            {
                cache_require(1);
                cache_fake(dialog_refresh_timer_addr, dialog_refresh_timer_new_instr, TYPE_ICACHE);
            }
            else /* unhack */
            {
                cache_fake(dialog_refresh_timer_addr, dialog_refresh_timer_orig_instr, TYPE_ICACHE);
                cache_require(0);
            }
        }
    }
}

static int FAST choose_next_capture_slot()
{
    /* keep on rolling? */
    /* O(1) */
    if (
        capture_slot >= 0 && 
        capture_slot + 1 < slot_count && 
        slots[capture_slot + 1].ptr == slots[capture_slot].ptr + slots[capture_slot].size && 
        slots[capture_slot + 1].status == SLOT_FREE &&
        !force_new_buffer
       )
        return capture_slot + 1;

    /* choose a new buffer? */
    /* choose the largest contiguous free section */
    /* O(n), n = slot_count */
    int len = 0;
    void* prev_ptr = PTR_INVALID;
    int prev_size = 0;
    int best_len = 0;
    int best_index = -1;
    for (int i = 0; i < slot_count; i++)
    {
        if (slots[i].status == SLOT_FREE)
        {
            if (slots[i].ptr == prev_ptr + prev_size)
            {
                len++;
                prev_ptr = slots[i].ptr;
                prev_size = slots[i].size;
                if (len > best_len)
                {
                    best_len = len;
                    best_index = i - len + 1;
                }
            }
            else
            {
                len = 1;
                prev_ptr = slots[i].ptr;
                prev_size = slots[i].size;
                if (len > best_len)
                {
                    best_len = len;
                    best_index = i;
                }
            }
        }
        else
        {
            len = 0;
            prev_ptr = PTR_INVALID;
        }
    }

    /* fixme: */
    /* avoid 32MB writes, they are slower (they require two DMA calls) */
    /* go back a few K and the speed is restored */
    //~ best_len = MIN(best_len, (32*1024*1024 - 8192) / max_frame_size);
    
    force_new_buffer = 0;

    return best_index;
}

static void shrink_slot(int slot_index, int new_frame_size)
{
    uint32_t old_int = cli();

    int i = slot_index;

    /* round to 512 multiples for file write speed - see frame_size_padded */
    int new_size = (VIDF_HDR_SIZE + new_frame_size + 4 + 511) & ~511;
    int old_size = slots[i].size;
    int dif_size = old_size - new_size;
    ASSERT(dif_size > 0);

    //printf("Shrink slot %d from %d to %d.\n", i, old_size, new_size);

    slots[i].size = new_size;
    slots[i].payload_size = new_frame_size;
    ((mlv_vidf_hdr_t*)slots[i].ptr)->blockSize
        = slots[i].size;

    int linked =
        (i+1 < COUNT(slots)) &&
        (slots[i+1].status == SLOT_FREE || slots[i+1].status == SLOT_RESERVED) &&
        (slots[i+1].ptr == slots[i].ptr + old_size);

    if (linked)
    {
        /* adjust the next slot from the same chunk (increase its size) */
        slots[i+1].ptr  -= dif_size;
        slots[i+1].size += dif_size;
        
        /* if it's big enough, mark it as available */
        if (slots[i+1].size >= max_frame_size)
        {
            if (slots[i+1].status == SLOT_RESERVED)
            {
                //printf("Slot %d becomes available (%d >= %d).\n", i, slots[i+1].size, max_frame_size);
            }
            else
            {
                /* existing free slots will get shifted, without changing their size */
                ASSERT(slots[i+1].size - dif_size == max_frame_size);
            }
            shrink_slot(i+1, frame_size_uncompressed);
            ASSERT(slots[i+1].size == max_frame_size);
            slots[i+1].status = SLOT_FREE;
        }
    }

    sei(old_int);
}

static void free_slot(int slot_index)
{
    int i = slot_index;

    slots[i].status = SLOT_RESERVED;
    
    if (slots[i].size == max_frame_size)
    {
        slots[i].status = SLOT_FREE;
        return;
    }

    ASSERT(slots[i].size < max_frame_size);

    /* re-allocate all reserved slots from this chunk to full frames */
    /* the remaining reserved slots will be moved at the end */

    /* this is called from both vsync and raw_rec_task */
    uint32_t old_int = cli();

    /* find first slot from this chunk */
    while ((i-1 >= 0) &&
           (slots[i-1].status == SLOT_FREE || slots[i-1].status == SLOT_RESERVED) &&
           (slots[i].ptr == slots[i-1].ptr + slots[i-1].size))
    {
        i--;
    }
    int start = i;

    /* find last slot from this chunk */
    i = slot_index;
    while ((i+1 < COUNT(slots)) &&
           (slots[i+1].status == SLOT_FREE || slots[i+1].status == SLOT_RESERVED) &&
           (slots[i+1].ptr == slots[i].ptr + slots[i].size))
    {
        i++;
    }
    int end = i;

    //printf("Reallocating slots %d...%d.\n", start, end);
    void * start_ptr = slots[start].ptr;
    void * end_ptr = slots[end].ptr + slots[end].size;
    void * ptr = start_ptr;
    for (i = start; i <= end; i++)
    {
        slots[i].ptr = ptr;

        if (ptr + max_frame_size <= end_ptr)
        {
            slots[i].status = SLOT_FREE;
            slots[i].size = max_frame_size;
        }
        else
        {
            /* first reserved slot will have non-zero size */
            /* all others 0 */
            slots[i].status = SLOT_RESERVED;
            slots[i].size = end_ptr - ptr;
            ASSERT(slots[i].size < max_frame_size);
        }
        ptr += slots[i].size;
    }

    sei(old_int);
}

static void pre_record_vsync_step()
{
    if (raw_recording_state == RAW_PRE_RECORDING)
    {
        if (pre_record_triggered)
        {
            /* queue all captured frames for writing */
            /* (they are numbered from 1 to frame_count-1; frame 0 is skipped) */
            /* they are not ordered, which complicates things a bit */
            int i = 0;
            for (int current_frame = 1; current_frame < frame_count; current_frame++)
            {
                /* consecutive frames tend to be grouped, 
                 * so this loop will not run every time */
                while (slots[i].status != SLOT_FULL || slots[i].frame_number != current_frame)
                {
                    i = MOD(i+1, slot_count);
                }
                
                writing_queue[writing_queue_tail] = i;
                writing_queue_tail = MOD(writing_queue_tail + 1, COUNT(writing_queue));
                i = MOD(i+1, slot_count);
            }
            
            /* done, from now on we can just record normally */
            raw_recording_state = RAW_RECORDING;
        }
        else if (frame_count >= pre_record_num_frames)
        {
            /* discard old frames */
            /* also adjust frame_count so all frames start from 1,
             * just like the rest of the code assumes */
            frame_count--;
            
            for (int i = 0; i < slot_count; i++)
            {
                /* first frame is 1 */
                if (slots[i].status == SLOT_FULL)
                {
                    ASSERT(slots[i].frame_number > 0);
                    
                    if (slots[i].frame_number == 1)
                    {
                        free_slot(i);
                    }
                    else
                    {
                        slots[i].frame_number--;
                        ((mlv_vidf_hdr_t*)slots[i].ptr)->frameNumber
                            = slots[i].frame_number - 1;
                    }
                }
            }
        }
    }
}

#define FRAME_SENTINEL 0xA5A5A5A5 /* for double-checking EDMAC operations */

static void frame_add_checks(int slot_index)
{
    void* ptr = slots[slot_index].ptr + VIDF_HDR_SIZE;
    uint32_t edmac_size = (slots[slot_index].payload_size + 3) & ~3;
    uint32_t* frame_end = ptr + edmac_size - 4;
    uint32_t* after_frame = ptr + edmac_size;
    *(volatile uint32_t*) frame_end = FRAME_SENTINEL; /* this will be overwritten by EDMAC */
    *(volatile uint32_t*) after_frame = FRAME_SENTINEL; /* this shalt not be overwritten */
}

static void frame_mark_complete(int slot_index)
{
    void* ptr = slots[slot_index].ptr + VIDF_HDR_SIZE;
    uint32_t edmac_size = (slots[slot_index].payload_size + 3) & ~3;
    uint32_t* after_frame = ptr + edmac_size;
    *(volatile uint32_t*) after_frame = FRAME_SENTINEL;
}

static int frame_check_saved(int slot_index)
{
    void* ptr = slots[slot_index].ptr + VIDF_HDR_SIZE;
    uint32_t edmac_size = (slots[slot_index].payload_size + 3) & ~3;
    uint32_t* frame_end = ptr + edmac_size - 4;
    uint32_t* after_frame = ptr + edmac_size;
    if (*(volatile uint32_t*) after_frame != FRAME_SENTINEL)
    {
        /* EDMAC overflow */
        return -1;
    }
    
    if (*(volatile uint32_t*) frame_end == FRAME_SENTINEL)
    {
        /* frame not yet complete */
        return 0;
    }
    
    /* looks alright */
    return 1;
}

static void edmac_cbr_r(void *ctx)
{
}

static void edmac_cbr_w(void *ctx)
{
    edmac_active = 0;
    edmac_copy_rectangle_adv_cleanup();
}

static struct msg_queue * compress_mq = 0;

static void compress_task()
{
    if (!compress_mq)
    {
        compress_mq = msg_queue_create("compress_mq", 1);
        ASSERT(compress_mq);
    }

    /* get exclusive access to our edmac channels */
    if (OUTPUT_COMPRESSION == 0)
    {
        edmac_memcpy_res_lock();
        printf("EDMAC copy resources locked.\n");
    }

    /* run as long as the main recorder task requires it */
    while (1)
    {
        uint32_t msg;
        msg_queue_receive(compress_mq, (struct event**)&msg, 0);

        if (msg == 0xFFFFFFFF)
        {
            /* request to stop */
            break;
        }

        int slot_index = msg & 0xFFFF;
        int fullsize_index = msg >> 16;

        void* out_ptr = slots[slot_index].ptr + VIDF_HDR_SIZE;
        void* fullSizeBuffer = fullsize_buffers[fullsize_index];

        if (OUTPUT_COMPRESSION)
        {
            struct memSuite * outSuite = CreateMemorySuite((void*)out_ptr, max_frame_size, 0);
            int compressed_size = lossless_compress_raw_rectangle(
                outSuite, fullSizeBuffer,
                raw_info.width, skip_x, skip_y,
                res_x, res_y
            );
            ASSERT(compressed_size < max_frame_size);
            DeleteMemorySuite(outSuite);
            
            /* resize frame slots on the fly, to compressed size */
            shrink_slot(slot_index, compressed_size);
            frame_mark_complete(slot_index);
        }
        else
        {
            edmac_active = 1;
            edmac_copy_rectangle_cbr_start(
                (void*)out_ptr, fullSizeBuffer,
                raw_info.pitch,
                (skip_x+7)/8*BPP, skip_y/2*2,
                res_x*BPP/8, 0, 0, res_x*BPP/8, res_y,
                &edmac_cbr_r, &edmac_cbr_w, NULL
            );
        }
    }

    /* exclusive edmac access no longer needed */
    if (OUTPUT_COMPRESSION == 0)
    {
        edmac_memcpy_res_unlock();
        printf("EDMAC copy resources unlocked.\n");
    }
}

static void FAST process_frame()
{
    /* skip the first frame, it will be gibberish */
    if (frame_count == 0)
    {
        frame_count++;
        return;
    }
    
    if (edmac_active)
    {
        /* EDMAC too slow */
        NotifyBox(2000, "EDMAC timeout.");
        buffer_full = 1;
        return;
    }
    
    if (raw_recording_state == RAW_PRE_RECORDING)
    {
        pre_record_vsync_step();
    }
    
    /* where to save the next frame? */
    capture_slot = choose_next_capture_slot(capture_slot);
    
    if (capture_slot >= 0)
    {
        /* okay */
        slots[capture_slot].frame_number = frame_count;
        slots[capture_slot].status = SLOT_FULL;
        frame_add_checks(capture_slot);

        if (raw_recording_state == RAW_PRE_RECORDING)
        {
            /* pre-recording before trigger? don't queue frames for writing */
            /* (do nothing here) */
        }
        else
        {
            /* send it for saving, even if it isn't done yet */
            /* it's quite unlikely that FIO DMA will be faster than EDMAC */
            writing_queue[writing_queue_tail] = capture_slot;
            writing_queue_tail = MOD(writing_queue_tail + 1, COUNT(writing_queue));
        }
    }
    else
    {
        /* card too slow */
        buffer_full = 1;
        return;
    }

    /* set VIDF metadata for this frame */
    vidf_hdr.frameNumber = slots[capture_slot].frame_number - 1;
    mlv_set_timestamp((mlv_hdr_t*)&vidf_hdr, mlv_start_timestamp);
    vidf_hdr.cropPosX = (skip_x + 7) & ~7;
    vidf_hdr.cropPosY = skip_y & ~1;
    vidf_hdr.panPosX = skip_x;
    vidf_hdr.panPosY = skip_y;
    *(mlv_vidf_hdr_t*)(slots[capture_slot].ptr) = vidf_hdr;

    /* advance to next buffer for the upcoming capture */
    fullsize_buffer_pos = (fullsize_buffer_pos + 1) % 2;

    //~ printf("saving frame %d: slot %d ptr %x\n", frame_count, capture_slot, ptr);

    /* copy current frame to our buffer and crop it to its final size */
    /* for some reason, compression cannot be started from vsync */
    /* let's delegate it to another task */
    ASSERT(compress_mq);
    msg_queue_post(compress_mq, capture_slot | (fullsize_buffer_pos << 16));

    /* advance to next frame */
    frame_count++;

    return;
}

static unsigned int FAST raw_rec_vsync_cbr(unsigned int unused)
{
    if (!raw_video_enabled) return 0;
    if (!is_movie_mode()) return 0;
    
    hack_liveview_vsync();
 
    /* panning window is updated when recording, but also when not recording */
    panning_update();

    if (!RAW_IS_RECORDING) return 0;
    if (!raw_lv_settings_still_valid()) { raw_recording_state = RAW_FINISHING; return 0; }
    if (buffer_full) return 0;

    /* double-buffering */
    raw_lv_redirect_edmac(fullsize_buffers[fullsize_buffer_pos % 2]);

    process_frame();

    return 0;
}

static const char* get_cf_dcim_dir()
{
    static char dcim_dir[FIO_MAX_PATH_LENGTH];
    struct card_info * card = get_shooting_card();
    if (is_dir("A:/")) card = get_card(CARD_A);
    snprintf(dcim_dir, sizeof(dcim_dir), "%s:/DCIM/%03d%s", card->drive_letter, card->folder_number, get_dcim_dir_suffix());
    return dcim_dir;
}

static char* get_next_raw_movie_file_name()
{
    static char filename[100];

    struct tm now;
    LoadCalendarFromRTC(&now);

    for (int number = 0 ; number < 100; number++)
    {
        /**
         * Get unique file names from the current date/time
         * last field gets incremented if there's another video with the same name
         */
        snprintf(filename, sizeof(filename), "%s/M%02d-%02d%02d.MLV", get_cf_dcim_dir(), now.tm_mday, now.tm_hour, COERCE(now.tm_min + number, 0, 99));
        
        /* already existing file? */
        uint32_t size;
        if( FIO_GetFileSize( filename, &size ) != 0 ) break;
        if (size == 0) break;
    }
    
    return filename;
}

static char* get_next_chunk_file_name(char* base_name, int chunk)
{
    static char filename[100];

    /* change file extension, according to chunk number: RAW, R00, R01 and so on */
    snprintf(filename, sizeof(filename), "%s", base_name);
    int len = strlen(filename);
    snprintf(filename + len - 2, 3, "%02d", chunk-1);
    
    return filename;
}

/* a bit of a hack: this tells the audio backend that we are going to record sound */
/* => it will show audio meters and disable beeps */
int mlv_snd_is_enabled()
{
    return h264_proxy && sound_recording_enabled_canon();
}

static void init_mlv_chunk_headers(struct raw_info * raw_info)
{
    mlv_start_timestamp = mlv_set_timestamp(NULL, 0);
    
    memset(&file_hdr, 0, sizeof(mlv_file_hdr_t));
    mlv_init_fileheader(&file_hdr);
    file_hdr.fileGuid = mlv_generate_guid();
    file_hdr.fileNum = 0;
    file_hdr.fileCount = 0; //autodetect
    file_hdr.fileFlags = 4;
    file_hdr.videoClass = 1;
    file_hdr.audioClass = 0;
    file_hdr.videoFrameCount = 0; //autodetect
    file_hdr.audioFrameCount = 0;
    file_hdr.sourceFpsNom = fps_get_current_x1000();
    file_hdr.sourceFpsDenom = 1000;
    
    memset(&rawi_hdr, 0, sizeof(mlv_rawi_hdr_t));
    mlv_set_type((mlv_hdr_t *)&rawi_hdr, "RAWI");
    mlv_set_timestamp((mlv_hdr_t *)&rawi_hdr, mlv_start_timestamp);
    rawi_hdr.blockSize = sizeof(mlv_rawi_hdr_t);
    rawi_hdr.xRes = res_x;
    rawi_hdr.yRes = res_y;
    rawi_hdr.raw_info = *raw_info;

    /* overwrite bpp relevant information */
    rawi_hdr.raw_info.bits_per_pixel = BPP;
    rawi_hdr.raw_info.pitch = rawi_hdr.raw_info.width * BPP / 8;

    /* scale black and white levels, minimizing the roundoff error */
    int black14 = rawi_hdr.raw_info.black_level;
    int white14 = rawi_hdr.raw_info.white_level;
    int bpp_scaling = (1 << (14 - BPP));
    rawi_hdr.raw_info.black_level = (black14 + bpp_scaling/2) / bpp_scaling;
    rawi_hdr.raw_info.white_level = (white14 + bpp_scaling/2) / bpp_scaling;

    mlv_fill_idnt(&idnt_hdr, mlv_start_timestamp);
    mlv_fill_expo(&expo_hdr, mlv_start_timestamp);
    mlv_fill_lens(&lens_hdr, mlv_start_timestamp);
    mlv_fill_rtci(&rtci_hdr, mlv_start_timestamp);
    mlv_fill_wbal(&wbal_hdr, mlv_start_timestamp);

    /* init MLV header for each frame (VIDF) */
    memset(&vidf_hdr, 0, sizeof(mlv_vidf_hdr_t));
    mlv_set_type((mlv_hdr_t*)&vidf_hdr, "VIDF");
    vidf_hdr.blockSize  = max_frame_size;
    vidf_hdr.frameSpace = VIDF_HDR_SIZE - sizeof(mlv_vidf_hdr_t);
}

static int write_mlv_chunk_headers(FILE* f)
{
    if (FIO_WriteFile(f, &file_hdr, file_hdr.blockSize) != (int)file_hdr.blockSize) return 0;
    if (FIO_WriteFile(f, &rawi_hdr, rawi_hdr.blockSize) != (int)rawi_hdr.blockSize) return 0;
    if (FIO_WriteFile(f, &idnt_hdr, idnt_hdr.blockSize) != (int)idnt_hdr.blockSize) return 0;
    if (FIO_WriteFile(f, &expo_hdr, expo_hdr.blockSize) != (int)expo_hdr.blockSize) return 0;
    if (FIO_WriteFile(f, &lens_hdr, lens_hdr.blockSize) != (int)lens_hdr.blockSize) return 0;
    if (FIO_WriteFile(f, &rtci_hdr, rtci_hdr.blockSize) != (int)rtci_hdr.blockSize) return 0;
    if (FIO_WriteFile(f, &wbal_hdr, wbal_hdr.blockSize) != (int)wbal_hdr.blockSize) return 0;
    int hdr_size = sizeof(file_hdr) + sizeof(rawi_hdr) + sizeof(idnt_hdr) + sizeof(expo_hdr) + sizeof(lens_hdr) + sizeof(rtci_hdr) + sizeof(wbal_hdr);
    
    /* insert a null block so the header size is multiple of 512 bytes */
    mlv_hdr_t nul_hdr;
    mlv_set_type(&nul_hdr, "NULL");
    int padded_size = (hdr_size + sizeof(nul_hdr) + 511) & ~511;
    nul_hdr.blockSize = padded_size - hdr_size;
    if (FIO_WriteFile(f, &nul_hdr, nul_hdr.blockSize) != (int)nul_hdr.blockSize) return 0;
    
    return padded_size;
}

static int file_size_limit = 0;         /* have we run into the 4GB limit? */
static int last_write_timestamp = 0;    /* last FIO_WriteFile call */
static int mlv_chunk = 0;               /* MLV chunk index from header */

/* update the frame count and close the chunk */
static void finish_chunk(FILE* f)
{
    file_hdr.videoFrameCount = chunk_frame_count;
    FIO_SeekSkipFile(f, 0, SEEK_SET);
    FIO_WriteFile(f, &file_hdr, file_hdr.blockSize);
    FIO_CloseFile(f);
    chunk_frame_count = 0;
}

/* This saves a group of frames, also taking care of file splitting if required */
static int write_frames(FILE** pf, void* ptr, int group_size, int num_frames)
{
    FILE* f = *pf;
    
    /* if we know there's a 4GB file size limit and we're about to exceed it, go ahead and make a new chunk */
    if (file_size_limit && written_chunk + group_size > 0xFFFFFFFF)
    {
        finish_chunk(f);
        chunk_filename = get_next_chunk_file_name(raw_movie_filename, ++mlv_chunk);
        printf("About to reach 4GB limit.\n");
        printf("Creating new chunk: %s\n", chunk_filename);
        FILE* g = FIO_CreateFile(chunk_filename);
        if (!g) return 0;
        
        file_hdr.fileNum = mlv_chunk;
        written_chunk = write_mlv_chunk_headers(g);
        written_total += written_chunk;
        
        if (written_chunk)
        {
            printf("Success!\n");
            *pf = f = g;
        }
        else
        {
            printf("New chunk didn't work. Card full?\n");
            FIO_CloseFile(g);
            FIO_RemoveFile(chunk_filename);
            mlv_chunk--;
            return 0;
        }
    }
    
    int t0 = get_ms_clock_value();
    if (!last_write_timestamp) last_write_timestamp = t0;
    idle_time += t0 - last_write_timestamp;
    int r = FIO_WriteFile(f, ptr, group_size);
    last_write_timestamp = get_ms_clock_value();

    if (r != group_size) /* 4GB limit or card full? */
    {
        printf("Write error.\n");
        
        /* failed, but not at 4GB limit, card must be full */
        if (written_chunk + group_size < 0xFFFFFFFF)
        {
            printf("Failed before 4GB limit. Card full?\n");
            /* don't try and write the remaining frames, the card is full */
            writing_queue_head = writing_queue_tail;
            return 0;
        }
        
        file_size_limit = 1;
        
        /* 5D2 does not write anything if the call failed, but 5D3 writes exactly 4294967295 */
        /* We need to write a null block to cover to the end of the file if anything was written */
        /* otherwise the file could end in the middle of a block */
        int64_t pos = FIO_SeekSkipFile(f, 0, SEEK_CUR);
        if (pos > written_chunk + 1)
        {
            printf("Covering incomplete block.\n");
            FIO_SeekSkipFile(f, written_chunk, SEEK_SET);
            mlv_hdr_t nul_hdr;
            mlv_set_type(&nul_hdr, "NULL");
            nul_hdr.blockSize = MAX(sizeof(nul_hdr), pos - written_chunk);
            FIO_WriteFile(f, &nul_hdr, sizeof(nul_hdr));
        }
        
        finish_chunk(f);
        /* try to create a new chunk */
        chunk_filename = get_next_chunk_file_name(raw_movie_filename, ++mlv_chunk);
        printf("Creating new chunk: %s\n", chunk_filename);
        FILE* g = FIO_CreateFile(chunk_filename);
        if (!g) return 0;
        
        file_hdr.fileNum = mlv_chunk;
        written_chunk = write_mlv_chunk_headers(g);
        written_total += written_chunk;
        
        int r2 = written_chunk ? FIO_WriteFile(g, ptr, group_size) : 0;
        if (r2 == group_size) /* new chunk worked, continue with it */
        {
            printf("Success!\n");
            *pf = f = g;
            written_total += group_size;
            written_chunk += group_size;
            chunk_frame_count += num_frames;
        }
        else /* new chunk didn't work, card full */
        {
            printf("New chunk didn't work. Card full?\n");
            FIO_CloseFile(g);
            FIO_RemoveFile(chunk_filename);
            mlv_chunk--;
            return 0;
        }
    }
    else
    {
        /* all fine */
        written_total += group_size;
        written_chunk += group_size;
        chunk_frame_count += num_frames;
    }
    
    writing_time += last_write_timestamp - t0;
    return 1;
}

static void setup_bit_depth()
{
    raw_lv_request_bpp(BPP);
}

static void restore_bit_depth()
{
    raw_lv_request_bpp(14);
}

static void raw_video_rec_task()
{
    //~ console_show();
    /* init stuff */
    raw_recording_state = RAW_PREPARING;
    slot_count = 0;
    capture_slot = -1;
    fullsize_buffer_pos = 0;
    frame_count = 0;
    chunk_frame_count = 0;
    buffer_full = 0;
    FILE* f = 0;
    written_total = 0; /* in bytes */
    int last_block_size = 0; /* for detecting early stops */
    last_write_timestamp = 0;
    mlv_chunk = 0;
    edmac_active = 0;
    pre_record_triggered = 0;

    uint32_t result = (uint32_t)
        task_create("compress_task", 0x0F, 0x1000, compress_task, (void*)0);
    ASSERT(!(result & 1));

    if (h264_proxy)
    {
        /* start H.264 recording */
        ASSERT(!RECORDING_H264);
        movie_start();
    }

    /* disable Canon's powersaving (30 min in LiveView) */
    powersave_prohibit();

    /* wait for two frames to be sure everything is refreshed */
    wait_lv_frames(2);
    
    /* detect raw parameters (geometry, black level etc) */
    raw_set_dirty();
    if (!raw_update_params())
    {
        NotifyBox(5000, "Raw detect error");
        goto cleanup;
    }
    
    update_resolution_params();

    /* create output file */
    raw_movie_filename = get_next_raw_movie_file_name();
    chunk_filename = raw_movie_filename;
    f = FIO_CreateFile(raw_movie_filename);
    if (!f)
    {
        NotifyBox(5000, "File create error");
        goto cleanup;
    }

    init_mlv_chunk_headers(&raw_info);
    written_total = written_chunk = write_mlv_chunk_headers(f);
    if (!written_chunk)
    {
        NotifyBox(5000, "Card Full");
        goto cleanup;
    }

    /* allocate memory */
    if (!setup_buffers())
    {
        NotifyBox(5000, "Memory error");
        goto cleanup;
    }

    hack_liveview(0);
    
    setup_bit_depth();

    /* this will enable the vsync CBR and the other task(s) */
    raw_recording_state = pre_record ? RAW_PRE_RECORDING : RAW_RECORDING;

    /* try a sync beep (not very precise, but better than nothing) */
    beep();

    /* signal that we are starting */
    raw_rec_cbr_starting();
    
    writing_time = 0;
    idle_time = 0;
    
    /* fake recording status, to integrate with other ml stuff (e.g. hdr video */
    set_recording_custom(CUSTOM_RECORDING_RAW);
    
    int fps = fps_get_current_x1000();
    
    int last_processed_frame = 0;
    
    /* main recording loop */
    while (RAW_IS_RECORDING && lv)
    {
        if (buffer_full)
        {
            goto abort_and_check_early_stop;
        }
        
        int w_tail = writing_queue_tail; /* this one can be modified outside the loop, so grab it here, just in case */
        int w_head = writing_queue_head; /* this one is modified only here, but use it just for the shorter name */

        /* writing queue empty? nothing to do */ 
        if (w_head == w_tail)
        {
            msleep(20);
            continue;
        }

        int first_slot = writing_queue[w_head];

        /* check whether the first frame was filled by EDMAC (it may be sent in advance) */
        /* probably not needed */
        int check = frame_check_saved(first_slot);
        if (check == 0)
        {
            msleep(20);
            continue;
        }

        /* group items from the queue in a contiguous block - as many as we can */
        int last_grouped = w_head;
        
        int group_size = 0;
        for (int i = w_head; i != w_tail; i = MOD(i+1, COUNT(writing_queue)))
        {
            int slot_index = writing_queue[i];

            /* TBH, I don't care if these are part of the same group or not,
             * as long as pointers are ordered correctly */
            if (slots[slot_index].ptr == slots[first_slot].ptr + group_size)
                last_grouped = i;
            else
                break;
            
            group_size += slots[slot_index].size;
        }
        
        /* grouped frames from w_head to last_grouped (including both ends) */
        int num_frames = MOD(last_grouped - w_head + 1, COUNT(writing_queue));
        
        int free_slots = get_free_slots();
        
        /* if we are about to overflow, save a smaller number of frames, so they can be freed quicker */
        if (measured_write_speed)
        {
            /* measured_write_speed unit: 0.01 MB/s */
            /* FPS unit: 0.001 Hz */
            /* overflow time unit: 0.1 seconds */
            int overflow_time = free_slots * 1000 * 10 / fps;
            /* better underestimate write speed a little */
            int avg_frame_size = group_size / num_frames;
            int frame_limit = overflow_time * 1024 / 10 * (measured_write_speed * 9 / 100) * 1024 / avg_frame_size / 10;
            if (frame_limit >= 0 && frame_limit < num_frames)
            {
                //~ printf("careful, will overflow in %d.%d seconds, better write only %d frames\n", overflow_time/10, overflow_time%10, frame_limit);
                num_frames = MAX(1, frame_limit - 1);
            }
        }
        
        int after_last_grouped = MOD(w_head + num_frames, COUNT(writing_queue));

        /* write queue empty? better search for a new larger buffer */
        if (after_last_grouped == writing_queue_tail)
        {
            force_new_buffer = 1;
        }

        void* ptr = slots[first_slot].ptr;

        /* mark these frames as "writing" */
        for (int i = w_head; i != after_last_grouped; i = MOD(i+1, COUNT(writing_queue)))
        {
            int slot_index = writing_queue[i];
            if (slots[slot_index].status != SLOT_FULL)
            {
                bmp_printf(FONT_LARGE, 30, 70, "Slot check error");
                beep();
            }
            slots[slot_index].status = SLOT_WRITING;
        }

        if (!write_frames(&f, ptr, group_size, num_frames))
        {
            goto abort;
        }

        /* for detecting early stops */
        last_block_size = MOD(after_last_grouped - w_head, COUNT(writing_queue));

        /* mark these frames as "free" so they can be reused */
        for (int i = w_head; i != after_last_grouped; i = MOD(i+1, COUNT(writing_queue)))
        {
            if (i == writing_queue_tail)
            {
                bmp_printf( FONT_MED, 30, 110, 
                    "Queue overflow"
                );
                beep();
            }
            
            int slot_index = writing_queue[i];

            if (frame_check_saved(slot_index) != 1)
            {
                bmp_printf( FONT_MED, 30, 110, 
                    "Data corruption at slot %d, frame %d ", slot_index, slots[slot_index].frame_number
                );
                beep();
            }
            
            if (slots[slot_index].frame_number != last_processed_frame + 1)
            {
                bmp_printf( FONT_MED, 30, 110, 
                    "Frame order error: slot %d, frame %d, expected %d ", slot_index, slots[slot_index].frame_number, last_processed_frame + 1
                );
                beep();
            }
            last_processed_frame++;

            free_slot(slot_index);
        }
        
        /* remove these frames from the queue */
        writing_queue_head = after_last_grouped;

        /* error handling */
        if (0)
        {
abort:
            last_block_size = 0; /* ignore early stop check */

abort_and_check_early_stop:

            if (last_block_size > 2)
            {
                bmp_printf( FONT_MED, 30, 90, 
                    "Early stop (%d). Didn't make it to estimated record time!.", last_block_size
                );
                beep_times(last_block_size);
            }
            else
            {
                bmp_printf( FONT_MED, 30, 90, 
                    "Movie recording stopped automagically         "
                );
                /* this is error beep, not audio sync beep */
                beep_times(2);
            }
            break;
        }
    }
    
    /* make sure the user doesn't rush to turn off the camera or something */
    gui_uilock(UILOCK_EVERYTHING);
    
    /* signal that we are stopping */
    raw_rec_cbr_stopping();
    
    /* done, this will stop the vsync CBR and the copying task */
    raw_recording_state = RAW_FINISHING;

    /* wait until the other tasks calm down */
    msleep(500);

    /* end the compression task */
    msg_queue_post(compress_mq, 0xFFFFFFFF);

    set_recording_custom(CUSTOM_RECORDING_NOT_RECORDING);

    /* write remaining frames */
    for (; writing_queue_head != writing_queue_tail; writing_queue_head = MOD(writing_queue_head + 1, COUNT(writing_queue)))
    {
        int slot_index = writing_queue[writing_queue_head];

        if (slots[slot_index].status != SLOT_FULL)
        {
            bmp_printf( FONT_MED, 30, 110, 
                "Slot %d: frame %d not saved ", slot_index, slots[slot_index].frame_number
            );
            beep();
        }

        if (frame_check_saved(slot_index) != 1)
        {
            bmp_printf( FONT_MED, 30, 110, 
                "Data corruption at slot %d, frame %d ", slot_index, slots[slot_index].frame_number
            );
            beep();
        }

        if (slots[slot_index].frame_number != last_processed_frame + 1)
        {
            bmp_printf( FONT_MED, 30, 110, 
                "Frame order error: slot %d, frame %d, expected %d ", slot_index, slots[slot_index].frame_number, last_processed_frame + 1
            );
            beep();
        }
        last_processed_frame++;

        slots[slot_index].status = SLOT_WRITING;
        if (indicator_display == INDICATOR_RAW_BUFFER) show_buffer_status();
        if (!write_frames(&f, slots[slot_index].ptr, slots[slot_index].size, 1))
        {
            NotifyBox(5000, "Card Full");
            beep();
            break;
        }
        free_slot(slot_index);
    }

    if (!written_total || !f)
    {
        bmp_printf( FONT_MED, 30, 110, 
            "Nothing saved, card full maybe."
        );
        beep_times(3);
        msleep(2000);
    }

cleanup:
    if (f) finish_chunk(f);
    if (!written_total)
    {
        FIO_RemoveFile(raw_movie_filename);
        raw_movie_filename = 0;
    }

    /* everything saved, we can unlock the buttons.
     * note: freeing SRM memory will also touch uilocks,
     * so it's best to call this before free_buffers */
    gui_uilock(UILOCK_NONE);

    free_buffers();
    
    #ifdef DEBUG_BUFFERING_GRAPH
    take_screenshot(SCREENSHOT_FILENAME_AUTO, SCREENSHOT_BMP);
    #endif
    
    restore_bit_depth();
    
    hack_liveview(1);
    redraw();
    
    /* re-enable powersaving  */
    powersave_permit();

    if (h264_proxy && RECORDING_H264)
    {
        /* stop H.264 recording */
        movie_end();
    }

    raw_recording_state = RAW_IDLE;
}

static void raw_start_stop()
{
    if (!RAW_IS_IDLE)
    {
        raw_recording_state = RAW_FINISHING;
        beep();
    }
    else
    {
        raw_recording_state = RAW_PREPARING;
        gui_stop_menu();
        task_create("raw_rec_task", 0x19, 0x1000, raw_video_rec_task, (void*)0);
    }
}

static MENU_SELECT_FUNC(raw_playback_start)
{
    if (RAW_IS_IDLE)
    {
        if (!raw_movie_filename)
        {
            bmp_printf(FONT_MED, 20, 50, "Please record a movie first.");
            return;
        }
        mlv_play_file(raw_movie_filename);
    }
}

static MENU_UPDATE_FUNC(raw_playback_update)
{
    if ((thunk)mlv_play_file == (thunk)ret_0)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "You need to load the mlv_play module.");
    
    if (raw_movie_filename)
        MENU_SET_VALUE(raw_movie_filename + 17);
    else
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING, "Record a video clip first.");
}

static struct menu_entry raw_video_menu[] =
{
    {
        .name = "RAW video",
        .priv = &raw_video_enabled,
        .max = 1,
        .update = raw_main_update,
        .submenu_width = 710,
        .depends_on = DEP_LIVEVIEW | DEP_MOVIE_MODE,
        .help = "Record RAW video (MLV format, no sound, basic metadata).",
        .help2 = "Press LiveView to start recording.",
        .children =  (struct menu_entry[]) {
            {
                .name = "Resolution",
                .priv = &resolution_index_x,
                .max = COUNT(resolution_presets_x) - 1,
                .select = resolution_change_fine_value,
                .update = resolution_update,
                .choices = RESOLUTION_CHOICES_X,
            },
            {
                .name = "Aspect ratio",
                .priv = &aspect_ratio_index,
                .max = COUNT(aspect_ratio_presets_num) - 1,
                .update = aspect_ratio_update,
                .choices = aspect_ratio_choices,
            },
            {
                .name       = "Data format",
                .priv       = &output_format,
                .max        = 3,
                .choices    = CHOICES(
                                "14-bit",
                                "12-bit",
                                "10-bit",
                                "14-bit lossless",
                                "12-bit lossless",
                                "10-bit lossless",
                              ),
                .help       = "Choose the output format (bit depth, compression) for the raw stream:",
                .help2      = "14-bit: native uncompressed format used in Canon firmware.\n"
                              "12-bit: uncompressed, 2 LSB trimmed (nearly lossless on current sensor).\n"
                              "10-bit: uncompressed, 4 LSB trimmed (small loss of detail in shadows).\n"
                              "14-bit lossless: compressed with Canon's Lossless JPEG (about 55-65%).\n"
                              "12-bit lossless: signal divided by 4 before compression (about 50-55%).\n"
                              "10-bit lossless: signal divided by 16 before compression (about 45-50%).\n",
            },
            {
                .name = "Preview",
                .priv = &preview_mode,
                .max = 3,
                .choices = CHOICES("Auto", "Canon", "ML Grayscale", "HaCKeD"),
                .help2 = "Auto: ML chooses what's best for each video mode\n"
                         "Canon: plain old LiveView. Framing is not always correct.\n"
                         "ML Grayscale: looks ugly, but at least framing is correct.\n"
                         "HaCKeD: try to squeeze a little speed by killing LiveView.\n",
                .advanced = 1,
            },
            {
                .name    = "Pre-record",
                .priv    = &pre_record,
                .max     = 4,
                .choices = CHOICES("OFF", "1 second", "2 seconds", "5 seconds", "10 seconds"),
                .help    = "Pre-records a few seconds of video into memory, discarding old frames.",
                .help2   = "Press REC twice: 1 - to start pre-recording, 2 - for normal recording.",
            },
            {
                .name = "Digital dolly",
                .priv = &dolly_mode,
                .max = 1,
                .help = "Smooth panning of the recording window (software dolly).",
                .help2 = "Use arrow keys (joystick) to move the window.",
                .advanced = 1,
            },
            {
                .name   = "H.264 proxy",
                .priv   = &h264_proxy,
                .max    = 1,
                .help   = "Record a H.264 video at the same time.",
                .help2  = "For best performance, record H.264 on SD and RAW on CF.",
                .advanced = 1,
            },
            {
                .name = "Card warm-up",
                .priv = &warm_up,
                .max = 7,
                .choices = CHOICES("OFF", "16 MB", "32 MB", "64 MB", "128 MB", "256 MB", "512 MB", "1 GB"),
                .help  = "Write a large file on the card at camera startup.",
                .help2 = "Some cards seem to get a bit faster after this.",
                .advanced = 1,
            },
            {
                .name = "Use SRM job memory",
                .priv = &use_srm_memory,
                .max = 1,
                .help = "Allocate memory from SRM job buffers",
                .advanced = 1,
            },
            {
                .name = "Small hacks",
                .priv = &small_hacks,
                .max = 1,
                .help  = "Slow down Canon GUI, disable auto exposure, white balance...",
                .advanced = 1,
            },
            {
                .name = "Show buffer graph",
                .priv = &show_graph,
                .max = 1,
                .help = "Displays a graph of the current buffer usage and expected frames.",
                .advanced = 1,
            },
            {
                .name = "Playback",
                .select = raw_playback_start,
                .update = raw_playback_update,
                .icon_type = IT_ACTION,
                .help = "Play back the last raw video clip.",
            },
            MENU_ADVANCED_TOGGLE,
            MENU_EOL,
        },
    }
};


static unsigned int raw_rec_keypress_cbr(unsigned int key)
{
    if (!raw_video_enabled)
        return 1;

    if (!is_movie_mode())
        return 1;

    /* keys are only hooked in LiveView */
    if (!liveview_display_idle() && !RECORDING_RAW)
        return 1;

    /* if you somehow managed to start recording H.264, let it stop */
    if (RECORDING_H264 && !h264_proxy)
        return 1;
    
    /* block the zoom key while recording */
    if (!RAW_IS_IDLE && key == MODULE_KEY_PRESS_ZOOMIN)
        return 0;

    /* start/stop recording with the LiveView key */
    int rec_key_pressed = (key == MODULE_KEY_LV || key == MODULE_KEY_REC);
    
    /* ... or SET on 5D2/50D */
    if (cam_50d || cam_5d2) rec_key_pressed = (key == MODULE_KEY_PRESS_SET);
    
    if (rec_key_pressed)
    {
        switch(raw_recording_state)
        {
            case RAW_IDLE:
            case RAW_RECORDING:
                raw_start_stop();
                break;
            
            case RAW_PRE_RECORDING:
                pre_record_triggered = 1;
                break;
        }
        return 0;
    }
    
    /* panning (with arrow keys) */
    if (FRAMING_PANNING)
    {
        switch (key)
        {
            case MODULE_KEY_PRESS_LEFT:
                frame_offset_delta_x -= 8;
                return 0;
            case MODULE_KEY_PRESS_RIGHT:
                frame_offset_delta_x += 8;
                return 0;
            case MODULE_KEY_PRESS_UP:
                frame_offset_delta_y -= 2;
                return 0;
            case MODULE_KEY_PRESS_DOWN:
                frame_offset_delta_y += 2;
                return 0;
            case MODULE_KEY_PRESS_DOWN_LEFT:
                frame_offset_delta_y += 2;
                frame_offset_delta_x -= 8;
                return 0;
            case MODULE_KEY_PRESS_DOWN_RIGHT:
                frame_offset_delta_y += 2;
                frame_offset_delta_x += 8;
                return 0;
            case MODULE_KEY_PRESS_UP_LEFT:
                frame_offset_delta_y -= 2;
                frame_offset_delta_x -= 8;
                return 0;
            case MODULE_KEY_PRESS_UP_RIGHT:
                frame_offset_delta_y -= 2;
                frame_offset_delta_x += 8;
                return 0;
            case MODULE_KEY_JOY_CENTER:
                /* first click stop the motion, second click center the window */
                if (frame_offset_delta_x || frame_offset_delta_y)
                {
                    frame_offset_delta_y = 0;
                    frame_offset_delta_x = 0;
                }
                else
                {
                    frame_offset_y = 0;
                    frame_offset_x = 0;
                }
        }
    }
    
    return 1;
}

static unsigned int raw_rec_keypress_cbr_raw(unsigned int raw_event)
{
    struct event * event = (struct event *) raw_event;

    if (h264_proxy)
    {
        if (IS_FAKE(event))
        {
            if (raw_recording_state == RAW_PREPARING ||
                raw_recording_state == RAW_FINISHING)
            {
                /* fake events (generated from ML) are not processed */
                /* they are probably for starting/stopping H.264 */
                return 1;
            }
        }
    }

    int key = module_translate_key(event->param, MODULE_KEY_PORTABLE);
    return raw_rec_keypress_cbr(key);
}

static int preview_dirty = 0;

static int raw_rec_should_preview(void)
{
    if (!raw_video_enabled) return 0;
    if (!is_movie_mode()) return 0;

    /* keep x10 mode unaltered, for focusing */
    if (lv_dispsize == 10) return 0;
    
    if (PREVIEW_AUTO)
        /* enable preview in x5 mode, since framing doesn't match */
        return lv_dispsize == 5;

    else if (PREVIEW_CANON)
        return 0;
    
    else if (PREVIEW_ML)
        return 1;
    
    else if (PREVIEW_HACKED)
        return RAW_IS_RECORDING || get_halfshutter_pressed() || lv_dispsize == 5;
    
    return 0;
}

static unsigned int raw_rec_update_preview(unsigned int ctx)
{
    /* just say whether we can preview or not */
    if (ctx == 0)
    {
        int enabled = raw_rec_should_preview();
        if (!enabled && preview_dirty)
        {
            /* cleanup the mess, if any */
            raw_set_dirty();
            preview_dirty = 0;
        }
        return enabled;
    }
    
    struct display_filter_buffers * buffers = (struct display_filter_buffers *) ctx;

    raw_previewing = 1;
    raw_set_preview_rect(skip_x, skip_y, res_x, res_y);
    raw_force_aspect_ratio_1to1();
    raw_preview_fast_ex(
        (void*)-1,
        PREVIEW_HACKED && RAW_RECORDING ? (void*)-1 : buffers->dst_buf,
        -1,
        -1,
        get_halfshutter_pressed() ? RAW_PREVIEW_COLOR_HALFRES : RAW_PREVIEW_GRAY_ULTRA_FAST
    );
    raw_previewing = 0;

    if (!RAW_IS_IDLE)
    {
        /* be gentle with the CPU, save it for recording (especially if the buffer is almost full) */
        //~ msleep(free_buffers <= 2 ? 2000 : used_buffers > 1 ? 1000 : 100);
        msleep(1000);
    }

    preview_dirty = 1;
    return 1;
}

static struct lvinfo_item info_items[] = {
    /* Top bar */
    {
        .name = "Rec. Status",
        .which_bar = LV_TOP_BAR_ONLY,
        .update = recording_status,
        .preferred_position = 50,
        .priority = 10,
    }
};

static unsigned int raw_rec_init()
{
    cam_eos_m = is_camera("EOSM", "2.0.2");
    cam_5d2   = is_camera("5D2",  "2.1.2");
    cam_50d   = is_camera("50D",  "1.0.9");
    cam_550d  = is_camera("550D", "1.0.9");
    cam_6d    = is_camera("6D",   "1.1.6");
    cam_600d  = is_camera("600D", "1.0.2");
    cam_650d  = is_camera("650D", "1.0.4");
    cam_7d    = is_camera("7D",   "2.0.3");
    cam_700d  = is_camera("700D", "1.1.4");
    cam_60d   = is_camera("60D",  "1.1.1");
    cam_500d  = is_camera("500D", "1.1.1");

    cam_5d3_113 = is_camera("5D3",  "1.1.3");
    cam_5d3_123 = is_camera("5D3",  "1.2.3");
    cam_5d3 = (cam_5d3_113 || cam_5d3_123);
    
    if (cam_5d2 || cam_50d)
    {
       raw_video_menu[0].help = "Record RAW video. Press SET to start.";
    }

    menu_add("Movie", raw_video_menu, COUNT(raw_video_menu));

    lvinfo_add_items (info_items, COUNT(info_items));

    /* some cards may like this */
    if (warm_up)
    {
        NotifyBox(100000, "Card warming up...");
        char warmup_filename[100];
        snprintf(warmup_filename, sizeof(warmup_filename), "%s/warmup.raw", get_dcim_dir());
        FILE* f = FIO_CreateFile(warmup_filename);
        if (f)
        {
            FIO_WriteFile(f, (void*)0x40000000, 8*1024*1024 * (1 << warm_up));
            FIO_CloseFile(f);
            FIO_RemoveFile(warmup_filename);
        }
        NotifyBoxHide();
    }

    lossless_init();

    return 0;
}

static unsigned int raw_rec_deinit()
{
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(raw_rec_init)
    MODULE_DEINIT(raw_rec_deinit)
MODULE_INFO_END()

MODULE_CBRS_START()
    MODULE_CBR(CBR_VSYNC, raw_rec_vsync_cbr, 0)
    MODULE_CBR(CBR_KEYPRESS_RAW, raw_rec_keypress_cbr_raw, 0)
    MODULE_CBR(CBR_SHOOT_TASK, raw_rec_polling_cbr, 0)
    MODULE_CBR(CBR_DISPLAY_FILTER, raw_rec_update_preview, 0)
MODULE_CBRS_END()

MODULE_CONFIGS_START()
    MODULE_CONFIG(raw_video_enabled)
    MODULE_CONFIG(resolution_index_x)
    MODULE_CONFIG(res_x_fine)    
    MODULE_CONFIG(aspect_ratio_index)
    MODULE_CONFIG(measured_write_speed)
    MODULE_CONFIG(pre_record)
    MODULE_CONFIG(dolly_mode)
    MODULE_CONFIG(preview_mode)
    MODULE_CONFIG(use_srm_memory)
    MODULE_CONFIG(small_hacks)
    MODULE_CONFIG(warm_up)
    MODULE_CONFIG(output_format)
    MODULE_CONFIG(h264_proxy)
MODULE_CONFIGS_END()