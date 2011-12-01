/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id: $
 *
 * Copyright (C) 2011 by Tomasz Moń
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


#include <SDL.h>
#include "button.h"
#include "buttonmap.h"

int key_to_button(int keyboard_button)
{
    int new_btn = BUTTON_NONE;
    switch (keyboard_button)
    {
        case SDLK_KP4:
        case SDLK_LEFT:
            new_btn = BUTTON_LEFT;
            break;
        case SDLK_KP6:
        case SDLK_RIGHT:
            new_btn = BUTTON_RIGHT;
            break;
        case SDLK_KP8:
        case SDLK_UP:
            new_btn = BUTTON_UP;
            break;
        case SDLK_KP2:
        case SDLK_DOWN:
            new_btn = BUTTON_DOWN;
            break;
        case SDLK_KP7:
            new_btn = BUTTON_PREV;
            break;
        case SDLK_PAGEUP:
        case SDLK_KP9:
            new_btn = BUTTON_NEXT;
            break;
        case SDLK_KP0:
            new_btn = BUTTON_POWER;
            break;
        case SDLK_KP5:
        case SDLK_SPACE:
        case SDLK_KP_ENTER:
        case SDLK_RETURN:
            new_btn = BUTTON_SELECT;
            break;
        case SDLK_KP_PLUS:
            new_btn = BUTTON_VOL_UP;
            break;
        case SDLK_KP_MINUS:
            new_btn = BUTTON_VOL_DOWN;
            break;
        case SDL_BUTTON_WHEELDOWN:
        case SDLK_KP3:
            new_btn = BUTTON_SCROLL_FWD;
            break;
        case SDL_BUTTON_WHEELUP:
        case SDLK_KP1:
            new_btn = BUTTON_SCROLL_BACK;
            break;
    }
    return new_btn;
}

struct button_map bm[] = {
    { SDLK_UP,          191, 505, 36, "Up" },
    { SDLK_DOWN,        191, 630, 36, "Down" },
    { SDLK_LEFT,        130, 568, 36, "Left" },
    { SDLK_RIGHT,       256, 568, 36, "Right" },
    { SDLK_KP7,         107, 443, 40, "Prev" },
    { SDLK_KP9,         271, 443, 40, "Next" },
    { SDLK_KP5,         191, 568, 36, "Select" },
    { SDLK_KP0,         220, 43,  30, "Power" },
    { SDLK_KP3,         231, 520, 20, "Scroll Fwd" },
    { SDLK_KP1,         149, 520, 20, "Scroll Back" },
    { SDLK_KP_MINUS,      3, 377, 50, "Volume -" },
    { SDLK_KP_PLUS,       6, 175, 50, "Volume +" },
    { 0, 0, 0, 0, "None" }
};