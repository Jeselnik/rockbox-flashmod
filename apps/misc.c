/***************************************************************************
 *             __________               __   ___.                  
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___  
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /  
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <   
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \  
 *                     \/            \/     \/    \/            \/ 
 * $Id$
 *
 * Copyright (C) 2002 by Daniel Stenberg
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdlib.h>
#include <ctype.h>
#include "lang.h"
#include "string.h"
#include "config.h"
#include "file.h"
#include "lcd.h"
#include "sprintf.h"
#include "errno.h"
#include "system.h"
#include "timefuncs.h"
#include "screens.h"
#include "talk.h"
#include "mpeg.h"
#include "mp3_playback.h"
#include "settings.h"
#include "ata.h"
#include "kernel.h"
#include "power.h"
#include "powermgmt.h"
#include "backlight.h"
#ifdef HAVE_MMC
#include "ata_mmc.h"
#endif

/* Format a large-range value for output, using the appropriate unit so that
 * the displayed value is in the range 1 <= display < 1000 (1024 for "binary"
 * units) if possible, and 3 significant digits are shown. If a buffer is
 * given, the result is snprintf()'d into that buffer, otherwise the result is
 * voiced.*/
char *output_dyn_value(char *buf, int buf_size, int value,
                       const unsigned char **units, bool bin_scale)
{
    int scale = bin_scale ? 1024 : 1000;
    int fraction = 0;
    int unit_no = 0;
    int i;
    char tbuf[5];

    while (value >= scale)
    {
        fraction = value % scale;
        value /= scale;
        unit_no++;
    }
    if (bin_scale)
        fraction = fraction * 1000 / 1024;

    if (value >= 100 || !unit_no)
        tbuf[0] = '\0';
    else if (value >= 10)
        snprintf(tbuf, sizeof(tbuf), "%01d", fraction / 100);
    else
        snprintf(tbuf, sizeof(tbuf), "%02d", fraction / 10);
    
    if (buf)
    {
        if (strlen(tbuf))
            snprintf(buf, buf_size, "%d%s%s%s", value, str(LANG_POINT),
                     tbuf, P2STR(units[unit_no]));
        else
            snprintf(buf, buf_size, "%d%s", value, P2STR(units[unit_no]));
    }
    else
    {
        /* strip trailing zeros from the fraction */
        for (i = strlen(tbuf) - 1; (i >= 0) && (tbuf[i] == '0'); i--)
            tbuf[i] = '\0';

        talk_number(value, true);
        if (strlen(tbuf))
        {
            talk_id(LANG_POINT, true);
            talk_spell(tbuf, true);
        }
        talk_id(P2ID(units[unit_no]), true);
    }
    return buf;
}

/* Read (up to) a line of text from fd into buffer and return number of bytes
 * read (which may be larger than the number of bytes stored in buffer). If
 * an error occurs, -1 is returned (and buffer contains whatever could be 
 * read). A line is terminated by a LF char. Neither LF nor CR chars are 
 * stored in buffer.
 */
int read_line(int fd, char* buffer, int buffer_size)
{
    int count = 0;
    int num_read = 0;
    
    errno = 0;

    while (count < buffer_size)
    {
        unsigned char c;

        if (1 != read(fd, &c, 1))
            break;
        
        num_read++;
            
        if ( c == '\n' )
            break;

        if ( c == '\r' )
            continue;

        buffer[count++] = c;
    }

    buffer[MIN(count, buffer_size - 1)] = 0;

    return errno ? -1 : num_read;
}

#ifdef HAVE_LCD_BITMAP
extern unsigned char lcd_framebuffer[LCD_HEIGHT/8][LCD_WIDTH];
static const unsigned char bmpheader[] =
{
    0x42, 0x4d, 0x3e, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3e, 0x00,
    0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0xc4, 0x0e, 0x00, 0x00, 0xc4, 0x0e, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0xee, 0x90, 0x00, 0x00, 0x00,
    0x00, 0x00
};

void screen_dump(void)
{
    int fh;
    int bx, by, ix, iy;
    int src_byte, src_mask, dst_mask;
    char filename[MAX_PATH];
    static unsigned char line_block[8][16];
    struct tm *tm = get_time();
    
    snprintf(filename, MAX_PATH, "/dump %04d-%02d-%02d %02d-%02d-%02d.bmp",
             tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    fh = creat(filename, O_WRONLY);
    if (fh < 0)
        return;

    write(fh, bmpheader, sizeof(bmpheader));

    /* BMP image goes bottom up */
    for (by = LCD_HEIGHT/8 - 1; by >= 0; by--)
    {
        memset(&line_block[0][0], 0, 8*16);

        for (bx = 0; bx < LCD_WIDTH/8; bx++)
        {
            dst_mask = 0x01;
            for (ix = 7; ix >= 0; ix--)
            {
                src_byte = lcd_framebuffer[by][8*bx+ix];
                src_mask = 0x01;
                for (iy = 7; iy >= 0; iy--)
                {
                    if (src_byte & src_mask)
                        line_block[iy][bx] |= dst_mask;
                    src_mask <<= 1;
                }
                dst_mask <<= 1;
            }
        }
        
        write(fh, &line_block[0][0], 8*16);
    }
    close(fh);
}
#endif

/* parse a line from a configuration file. the line format is:

   name: value

   Any whitespace before setting name or value (after ':') is ignored.
   A # as first non-whitespace character discards the whole line.
   Function sets pointers to null-terminated setting name and value.
   Returns false if no valid config entry was found.
*/

bool settings_parseline(char* line, char** name, char** value)
{
    char* ptr;

    while ( isspace(*line) )
        line++;

    if ( *line == '#' )
        return false;

    ptr = strchr(line, ':');
    if ( !ptr )
        return false;

    *name = line;
    *ptr = 0;
    ptr++;
    while (isspace(*ptr))
        ptr++;
    *value = ptr;
    return true;
}

bool clean_shutdown(void)
{
#ifdef SIMULATOR
    exit(0);
#else
#ifndef IRIVER_H100
    if(!charger_inserted())
#endif
    {
        lcd_clear_display();
        splash(0, true, str(LANG_SHUTTINGDOWN));
        shutdown_hw();
    }
#endif
    return false;
}

long default_event_handler_ex(long event, void (*callback)(void *), void *parameter)
{
    switch(event)
    {
        case SYS_USB_CONNECTED:
            if (callback != NULL)
                callback(parameter);
#ifdef HAVE_MMC
            if (!mmc_detect() || (mmc_remove_request() == SYS_MMC_EXTRACTED))
#endif
                usb_screen();
            return SYS_USB_CONNECTED;
        case SYS_POWEROFF:
            if (callback != NULL)
                callback(parameter);
            if (!clean_shutdown())
                return SYS_POWEROFF;
            break;
    }
    return 0;
}

long default_event_handler(long event)
{
    return default_event_handler_ex(event, NULL, NULL);
}

