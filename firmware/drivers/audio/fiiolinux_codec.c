/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 *
 * Copyright (c) 2018 Marcin Bukat
 * Copyright (c) 2019 Roman Stolyarov
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

#include "config.h"
#include "audio.h"
#include "audiohw.h"
#include "system.h"
#include "kernel.h"
#include "panic.h"
#include "sysfs.h"
#include "alsa-controls.h"
#include "pcm-alsa.h"
#include <sys/ioctl.h>

static int fd_hw;
static int ak_hw;

static      int vol_sw[2] = {0};
static long int vol_hw[2] = {0};

static void hw_open(void)
{
    fd_hw = open("/dev/snd/controlC0", O_RDWR);
    if(fd_hw < 0)
        panicf("Cannot open '/dev/snd/controlC0'");

    ak_hw = open("/dev/ak4376", O_RDWR);
    if(ak_hw < 0)
        panicf("Cannot open '/dev/ak4376'");

    if(ioctl(ak_hw, 0x20003424, 0) < 0)
    {
        panicf("Call cmd AK4376_POWER_ON fail");
    }
}

static void hw_close(void)
{
    if(ioctl(ak_hw, 0x20003425, 0) < 0)
    {
        panicf("Call cmd AK4376_POWER_OFF fail");
    }
    close(ak_hw);

    close(fd_hw);
}

void audiohw_preinit(void)
{
    alsa_controls_init();
    hw_open();
}

void audiohw_postinit(void)
{
}

void audiohw_close(void)
{
    hw_close();
    alsa_controls_close();
}

void audiohw_set_frequency(int fsel)
{
    (void)fsel;
}

void audiohw_set_volume(int vol_l, int vol_r)
{
    int vol[2];
   
    vol[0] = vol_l / 20;
    vol[1] = vol_r / 20;

    for (int i = 0; i < 2; i++)
    {
        if (vol[i] > -56)
        {
            if (vol[i] < -12)
            {
                vol_hw[i] = 1;
                vol_sw[i] = vol[i] + 12;
            }
            else
            {
                vol_hw[i] = 25 - (-vol[i] * 2);
                vol_sw[i] = 0;
            }
        }
        else
        {
            // Mute
            vol_hw[i] = 0;
            vol_sw[i] = 0;
        }
    }

    alsa_controls_set_ints("DACL Playback Volume", 1, &vol_hw[0]);
    alsa_controls_set_ints("DACR Playback Volume", 1, &vol_hw[1]);
    pcm_alsa_set_digital_volume(vol_sw[0], vol_sw[1]);
}

void audiohw_mute(int mute)
{
    long int vol0 = 0;

    if(mute)
    {
        alsa_controls_set_ints("DACL Playback Volume", 1, &vol0);
        alsa_controls_set_ints("DACR Playback Volume", 1, &vol0);
        pcm_alsa_set_digital_volume(0, 0);
    }
    else
    {
        alsa_controls_set_ints("DACL Playback Volume", 1, &vol_hw[0]);
        alsa_controls_set_ints("DACR Playback Volume", 1, &vol_hw[1]);
        pcm_alsa_set_digital_volume(vol_sw[0], vol_sw[1]);
    }
}

void audiohw_set_filter_roll_off(int value)
{
    /* 0 = Sharp;
       1 = Slow;
       2 = Short Sharp
       3 = Short Slow */
#if defined(FIIO_M3K)
    long int value_hw = value;
    alsa_controls_set_ints("AK4376 Digital Filter", 1, &value_hw);
#else
    (void)value;
#endif
}
