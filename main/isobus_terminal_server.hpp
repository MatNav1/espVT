#pragma once

#include <cstdint>

namespace isovt
{
bool start_local_ui();
bool start();
void on_machine_inactive(std::uint64_t name, std::uint8_t address);
void request_aux_n_status_render();
bool on_touch_pressed(std::uint16_t x, std::uint16_t y);
void on_touch_released();
}
