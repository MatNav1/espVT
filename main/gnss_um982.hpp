#pragma once

#include <cstdint>

#include "esp_err.h"

namespace gnss_um982
{
enum class FixQuality : std::uint8_t
{
    Unavailable,
    NoFix,
    Autonomous,
    Differential,
    RtkFloat,
    RtkFixed,
};

struct Snapshot
{
    bool receiver_configured = false;
    bool position_valid = false;
    bool ntrip_enabled = false;
    bool ntrip_connected = false;
    double latitude_degrees = 0.0;
    double longitude_degrees = 0.0;
    float speed_mps = 0.0F;
    float course_degrees = 0.0F;
    float hdop = 0.0F;
    std::uint8_t satellites = 0;
    FixQuality fix_quality = FixQuality::Unavailable;
    std::int64_t last_position_ms = 0;
    std::int64_t last_velocity_ms = 0;
    std::int64_t last_rtcm_ms = 0;
    std::uint32_t rtcm_bytes = 0;
};

bool is_enabled();
esp_err_t start();
Snapshot get_snapshot();
bool is_section_control_position_ready();
const char *fix_quality_text(FixQuality quality);
}
