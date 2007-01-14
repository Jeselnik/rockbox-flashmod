/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 Itai Shaked 
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 **************************************************************************/
#include "plugin.h"
#include "playergfx.h"

PLUGIN_HEADER

#ifdef HAVE_LCD_BITMAP
#define MYLCD(fn) rb->lcd_ ## fn
#define GFX_X (LCD_WIDTH/2-1)
#define GFX_Y (LCD_HEIGHT/2-1)
#if LCD_WIDTH != LCD_HEIGHT
#define GFX_WIDTH  GFX_X
#define GFX_HEIGHT GFX_Y
#else
#define GFX_WIDTH  GFX_X
#define GFX_HEIGHT (4*GFX_Y/5)
#endif
#else
#define MYLCD(fn) pgfx_ ## fn
#define GFX_X 9
#define GFX_Y 6
#define GFX_WIDTH  9
#define GFX_HEIGHT 6
#endif

/* variable button definitions */
#if CONFIG_KEYPAD == RECORDER_PAD
#define MOSAIQUE_QUIT BUTTON_OFF
#define MOSAIQUE_SPEED BUTTON_F1
#define MOSAIQUE_RESTART BUTTON_PLAY

#elif CONFIG_KEYPAD == ARCHOS_AV300_PAD
#define MOSAIQUE_QUIT BUTTON_OFF
#define MOSAIQUE_SPEED BUTTON_F1
#define MOSAIQUE_RESTART BUTTON_SELECT

#elif CONFIG_KEYPAD == PLAYER_PAD
#define MOSAIQUE_QUIT BUTTON_STOP
#define MOSAIQUE_SPEED BUTTON_MENU
#define MOSAIQUE_RESTART BUTTON_PLAY

#elif CONFIG_KEYPAD == ONDIO_PAD
#define MOSAIQUE_QUIT BUTTON_OFF
#define MOSAIQUE_SPEED BUTTON_MENU
#define MOSAIQUE_RESTART BUTTON_RIGHT

#elif (CONFIG_KEYPAD == IRIVER_H100_PAD) || \
      (CONFIG_KEYPAD == IRIVER_H300_PAD)
#define MOSAIQUE_QUIT BUTTON_OFF
#define MOSAIQUE_SPEED BUTTON_MODE
#define MOSAIQUE_RESTART BUTTON_ON

#define MOSAIQUE_RC_QUIT BUTTON_RC_STOP

#elif (CONFIG_KEYPAD == IPOD_4G_PAD) || \
      (CONFIG_KEYPAD == IPOD_3G_PAD)
#define MOSAIQUE_QUIT BUTTON_MENU
#define MOSAIQUE_SPEED BUTTON_SELECT
#define MOSAIQUE_RESTART BUTTON_PLAY

#elif CONFIG_KEYPAD == IRIVER_IFP7XX_PAD
#define MOSAIQUE_QUIT BUTTON_PLAY
#define MOSAIQUE_SPEED BUTTON_MODE
#define MOSAIQUE_RESTART BUTTON_SELECT

#elif CONFIG_KEYPAD == IAUDIO_X5_PAD
#define MOSAIQUE_QUIT BUTTON_POWER
#define MOSAIQUE_SPEED BUTTON_SELECT
#define MOSAIQUE_RESTART BUTTON_PLAY

#elif CONFIG_KEYPAD == GIGABEAT_PAD
#define MOSAIQUE_QUIT BUTTON_A
#define MOSAIQUE_SPEED BUTTON_MENU
#define MOSAIQUE_RESTART BUTTON_SELECT

#elif CONFIG_KEYPAD == SANSA_E200_PAD
#define MOSAIQUE_QUIT BUTTON_POWER
#define MOSAIQUE_SPEED BUTTON_DOWN
#define MOSAIQUE_RESTART BUTTON_SELECT

#elif CONFIG_KEYPAD == IRIVER_H10_PAD
#define MOSAIQUE_QUIT BUTTON_POWER
#define MOSAIQUE_SPEED BUTTON_FF
#define MOSAIQUE_RESTART BUTTON_PLAY

#endif

enum plugin_status plugin_start(struct plugin_api* api, void* parameter)
{
    int button;
    int timer = 10;
    int x=0;
    int y=0;
    int sx = 3;
    int sy = 3;
    struct plugin_api* rb = api;
    (void)parameter;

#ifdef HAVE_LCD_CHARCELLS
    if (!pgfx_init(rb, 4, 2))
    {
        rb->splash(HZ*2, true, "Old LCD :(");
        return PLUGIN_OK;
    }
    pgfx_display(3, 0);
#endif
    MYLCD(clear_display)();
    MYLCD(set_drawmode)(DRMODE_COMPLEMENT);
    while (1) {

        x+=sx;
        if (x>GFX_WIDTH) 
        {
            x = 2*GFX_WIDTH-x;
            sx=-sx;
        }
	
        if (x<0) 
        {
            x = -x;
            sx = -sx;
        }
	
        y+=sy;
        if (y>GFX_HEIGHT) 
        {
            y = 2*GFX_HEIGHT-y;
            sy=-sy;
        }
	
        if (y<0) 
        {
            y = -y;
            sy = -sy;
        }

        MYLCD(fillrect)(GFX_X-x, GFX_Y-y, 2*x+1, 1);
        MYLCD(fillrect)(GFX_X-x, GFX_Y+y, 2*x+1, 1);
        MYLCD(fillrect)(GFX_X-x, GFX_Y-y+1, 1, 2*y-1);
        MYLCD(fillrect)(GFX_X+x, GFX_Y-y+1, 1, 2*y-1);
        MYLCD(update)();

        rb->sleep(HZ/timer);
        
        button = rb->button_get(false);
        switch (button)
        {
#ifdef MOSAIQUE_RC_QUIT
            case MOSAIQUE_RC_QUIT:
#endif
            case MOSAIQUE_QUIT:
                MYLCD(set_drawmode)(DRMODE_SOLID);
#ifdef HAVE_LCD_CHARCELLS
                pgfx_release();
#endif
                return PLUGIN_OK;

            case MOSAIQUE_SPEED:
                timer = timer+5;
                if (timer>20)
                    timer=5;
                break;
                
            case MOSAIQUE_RESTART:

                sx = rb->rand() % (GFX_HEIGHT/2) + 1;
                sy = rb->rand() % (GFX_HEIGHT/2) + 1;
                x=0;
                y=0;
                MYLCD(clear_display)();
                break;


            default:
                if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
                {
                    MYLCD(set_drawmode)(DRMODE_SOLID);
#ifdef HAVE_LCD_CHARCELLS
                    pgfx_release();
#endif
                    return PLUGIN_USB_CONNECTED;
                }
                break;
        }
    }
}

