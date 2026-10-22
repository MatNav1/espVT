#pragma once

#include <cstdint>
#include <string>

namespace vt_view
{
void init();
void set_status(const std::string &text);
void set_bus_state(bool can_up, bool agisostack_up);
void set_can_diagnostics(const std::string &text);
void set_can_signal_diagnostics(const std::string &text);
void set_gnss_status(const std::string &text);
void set_section_control_status(const std::string &text);
void set_object_pool_download_progress(bool active, std::uint8_t percent);
void on_machine_ecu_state(std::uint64_t name, std::uint8_t address, bool online);
void on_object_pool_loaded(std::uint64_t client_name, std::size_t bytes);
void on_repaint_requested(std::uint64_t client_name);
void on_active_mask_changed(std::uint64_t client_name, std::uint16_t previous_mask, std::uint16_t active_mask);
void on_soft_key_mask_changed(std::uint64_t client_name, std::uint16_t previous_mask, std::uint16_t active_mask);
void on_focus_changed(std::uint64_t client_name, std::uint16_t object_id, bool input_open);
void on_aux_n_input_device(std::uint64_t device_name, std::uint16_t model_identification_code);
void on_aux_n_assignment_result(std::uint64_t implement_name, std::uint16_t function_object_id, std::uint8_t error_code);
void on_touch(std::uint16_t x, std::uint16_t y, std::size_t count);
bool is_idle_menu_touch(std::uint16_t x, std::uint16_t y);
void set_working_set_visible(bool visible);
}
