#pragma once

#include "esp_err.h"

#include <cstdint>

namespace vt_audio
{
esp_err_t start();
bool is_enabled();
void play_control_signal(std::uint8_t activations,
                         std::uint16_t frequency_hz,
                         std::uint16_t duration_ms,
                         std::uint16_t off_time_ms);
void set_control_volume(std::uint8_t volume_percent);
void set_requested_control_volume(std::uint8_t volume_percent);
void set_alarm_volume(std::uint8_t volume_percent);
void set_muted(bool muted);
std::uint8_t get_control_volume();
std::uint8_t get_alarm_volume();
bool is_muted();
void play_test_control();
void play_test_alarm();
void set_alarm_signal(std::uint8_t acoustic_signal);
}
