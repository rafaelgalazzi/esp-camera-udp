#pragma once
#include "esp_camera.h"
#include "esp_log.h"

// Sensor values from EyeTrackVR/OpenIris-ESPIDF CameraManager::setupCameraSensor,
// commit b64b0fa84ce0d1b073067f8758399d76033340ad. JPEG remains sensor-encoded.
inline bool apply_ov2640_profile(sensor_t *sensor)
{
    if (!sensor || sensor->id.PID != OV2640_PID)
        return false;
    bool ok = true;
    const auto check = [&](int result, const char *setting) {
        if (result != 0) {
            ESP_LOGE("iris_udp", "Sensor setting failed: %s", setting);
            ok = false;
        }
    };
    check(sensor->set_reg(sensor, 0xff, 0xff, 0x00), "DSP bank");
    check(sensor->set_reg(sensor, 0xd3, 0xff, 5), "JPEG clock");
    check(sensor->set_brightness(sensor, 2), "brightness");
    check(sensor->set_contrast(sensor, 2), "contrast");
    check(sensor->set_saturation(sensor, -2), "saturation");
    check(sensor->set_whitebal(sensor, 1), "white balance");
    check(sensor->set_awb_gain(sensor, 0), "AWB gain");
    check(sensor->set_wb_mode(sensor, 0), "WB mode");
    check(sensor->set_exposure_ctrl(sensor, 0), "manual exposure");
    check(sensor->set_aec2(sensor, 0), "AEC2");
    check(sensor->set_ae_level(sensor, 0), "AE level");
    check(sensor->set_aec_value(sensor, 300), "exposure");
    check(sensor->set_gain_ctrl(sensor, 0), "manual gain");
    check(sensor->set_agc_gain(sensor, 2), "gain");
    check(sensor->set_gainceiling(sensor, static_cast<gainceiling_t>(6)), "gain ceiling");
    check(sensor->set_bpc(sensor, 1), "black pixel correction");
    check(sensor->set_wpc(sensor, 1), "white pixel correction");
    check(sensor->set_dcw(sensor, 0), "DCW");
    check(sensor->set_raw_gma(sensor, 1), "gamma");
    check(sensor->set_lenc(sensor, 0), "lens correction");
    check(sensor->set_colorbar(sensor, 0), "color bars");
    check(sensor->set_special_effect(sensor, 2), "grayscale");
    check(sensor->set_framesize(sensor, FRAMESIZE_240X240), "frame size");
    return ok;
}
