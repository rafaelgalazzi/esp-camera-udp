#pragma once
#include "esp_camera.h"
#include "settings.hpp"
inline camera_config_t camera_config() {
    camera_config_t c{};
    c.pin_pwdn=-1; c.pin_reset=-1;
    c.pin_xclk=10; c.pin_sccb_sda=40; c.pin_sccb_scl=39;
    c.pin_d0=15; c.pin_d1=17; c.pin_d2=18; c.pin_d3=16;
    c.pin_d4=14; c.pin_d5=12; c.pin_d6=11; c.pin_d7=48;
    c.pin_vsync=38; c.pin_href=47; c.pin_pclk=13;
    c.xclk_freq_hz=camera_xclk_hz;
    c.ledc_timer=LEDC_TIMER_0; c.ledc_channel=LEDC_CHANNEL_0;
    c.pixel_format=PIXFORMAT_JPEG; c.frame_size=FRAMESIZE_240X240;
    c.jpeg_quality=camera_jpeg_quality; c.fb_count=2;
    c.fb_location=CAMERA_FB_IN_PSRAM; c.grab_mode=CAMERA_GRAB_LATEST;
    return c;
}
