#include "lvgl_terminal_view.hpp"

#include "board_pins.hpp"
#include "lcd_panel.hpp"
#include "logo_image.hpp"

#include <cstdio>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace
{
constexpr char TAG[] = "vt_view";

constexpr std::uint16_t COLOR_BACKGROUND = 0xFFFF;
constexpr std::uint16_t COLOR_HEADER = 0x10A2;
constexpr std::uint16_t COLOR_ACCENT = 0x3C86;
constexpr std::uint16_t COLOR_OK = 0x05E0;
constexpr std::uint16_t COLOR_MUTED = 0x52AA;
constexpr std::uint16_t COLOR_TEXT = 0xFFFF;
constexpr std::uint16_t COLOR_DARK_TEXT = 0x10A2;
constexpr int MENU_BUTTON_X = 16;
constexpr int MENU_BUTTON_Y = 110;
constexpr int MENU_BUTTON_WIDTH = 64;
constexpr int MENU_BUTTON_HEIGHT = 196;

SemaphoreHandle_t view_mutex = nullptr;
TaskHandle_t render_task_handle = nullptr;
bool panel_ready = false;
bool can_ready = false;
bool stack_ready = false;
bool working_set_visible = false;
bool object_pool_download_active = false;
std::uint8_t object_pool_download_percent = 0;
std::string status = "STARTING";
std::string can_diagnostics = "CAN DIAGNOSTICS STARTING";
std::string can_signal_diagnostics = "CAN SIGNAL DIAGNOSTICS STARTING";
std::string gnss_status = "GNSS OPTIONAL MODULE OFF";
std::string section_control_status = "SECTION GEO: GNSS INPUT OFF";

void render_locked()
{
    if (!panel_ready || working_set_visible)
    {
        return;
    }

    const esp_err_t begin_result = lcd_panel::begin_frame();
    if (begin_result != ESP_OK)
    {
        ESP_LOGW(TAG, "Terminal frame begin failed: %s", esp_err_to_name(begin_result));
        return;
    }

    lcd_panel::fill(COLOR_BACKGROUND);
    lcd_panel::fill_rect(0, 0, board::LCD_WIDTH, 58, COLOR_HEADER);
    lcd_panel::draw_text(32, 18, 3, "ISOBUS ACCESS TERMINAL", COLOR_TEXT);
    lcd_panel::draw_text(764, 18, 2, can_ready ? "CAN READY" : "CAN STARTING", can_ready ? COLOR_OK : COLOR_TEXT);

    constexpr int logo_area_top = 72;
    constexpr int logo_area_bottom = 382;
    const int logo_top = logo_area_top + (logo_area_bottom - logo_area_top - logo_image::height()) / 2;
    logo_image::draw_centered(logo_top);

    lcd_panel::fill_rect(MENU_BUTTON_X, MENU_BUTTON_Y, MENU_BUTTON_WIDTH, MENU_BUTTON_HEIGHT, COLOR_HEADER);
    lcd_panel::fill_rect(MENU_BUTTON_X + 18, MENU_BUTTON_Y + 18, 28, 4, COLOR_TEXT);
    lcd_panel::fill_rect(MENU_BUTTON_X + 18, MENU_BUTTON_Y + 29, 28, 4, COLOR_TEXT);
    lcd_panel::fill_rect(MENU_BUTTON_X + 18, MENU_BUTTON_Y + 40, 28, 4, COLOR_TEXT);
    lcd_panel::draw_text(MENU_BUTTON_X + 26, MENU_BUTTON_Y + 65, 2, "M", COLOR_TEXT);
    lcd_panel::draw_text(MENU_BUTTON_X + 26, MENU_BUTTON_Y + 93, 2, "E", COLOR_TEXT);
    lcd_panel::draw_text(MENU_BUTTON_X + 26, MENU_BUTTON_Y + 121, 2, "N", COLOR_TEXT);
    lcd_panel::draw_text(MENU_BUTTON_X + 26, MENU_BUTTON_Y + 149, 2, "U", COLOR_TEXT);

    lcd_panel::fill_rect(42, 394, board::LCD_WIDTH - 84, 3, COLOR_ACCENT);
    if (object_pool_download_active)
    {
        constexpr int progress_x = 42;
        constexpr int progress_y = 448;
        constexpr int progress_width = 820;
        constexpr int progress_height = 30;
        const int filled_width = ((progress_width - 6) * object_pool_download_percent) / 100;
        char percent_text[8];
        std::snprintf(percent_text, sizeof(percent_text), "%u%%", static_cast<unsigned>(object_pool_download_percent));

        lcd_panel::draw_text(42, 416, 2, "LOADING MACHINE DATA", COLOR_MUTED);
        lcd_panel::fill_rect(progress_x, progress_y, progress_width, progress_height, COLOR_MUTED);
        lcd_panel::fill_rect(progress_x + 3, progress_y + 3, progress_width - 6, progress_height - 6, COLOR_BACKGROUND);
        if (filled_width > 0)
        {
            lcd_panel::fill_rect(progress_x + 3, progress_y + 3, filled_width, progress_height - 6, COLOR_ACCENT);
        }
        lcd_panel::draw_text(890, 453, 2, percent_text, COLOR_DARK_TEXT);
        lcd_panel::draw_text(42, 494, 2, status.c_str(), COLOR_MUTED);
    }
    else
    {
        lcd_panel::draw_text(42, 416, 2, "STATUS", COLOR_MUTED);
        lcd_panel::draw_text(42, 446, 2, status.c_str(), COLOR_DARK_TEXT);
        lcd_panel::draw_text(42, 494, 2, stack_ready ? "VT SERVER READY   AUX-N AVAILABLE" : "WAITING FOR MACHINE CONNECTION", COLOR_MUTED);
    }
    lcd_panel::draw_text(42, 522, 2, can_diagnostics.c_str(), COLOR_MUTED);
    lcd_panel::draw_text(42, 548, 2, can_signal_diagnostics.c_str(), COLOR_MUTED);
    lcd_panel::draw_text(42, 576, 1, gnss_status.c_str(), COLOR_MUTED);

    const esp_err_t present_result = lcd_panel::present();
    if (present_result != ESP_OK)
    {
        ESP_LOGW(TAG, "Terminal frame present failed: %s", esp_err_to_name(present_result));
    }
}

void request_render()
{
    if ((render_task_handle != nullptr) && panel_ready)
    {
        xTaskNotifyGive(render_task_handle);
    }
}

void render_task(void *)
{
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if ((view_mutex != nullptr) && (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(100)) == pdTRUE))
        {
            render_locked();
            xSemaphoreGive(view_mutex);
        }
    }
}

void render()
{
    request_render();
}

void update_status_and_render(const std::string &next_status)
{
    if (view_mutex == nullptr)
    {
        return;
    }

    if (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        status = next_status;
        xSemaphoreGive(view_mutex);
        request_render();
    }
}

void update_bus_state_and_render(bool next_can_ready, bool next_stack_ready)
{
    if (view_mutex == nullptr)
    {
        return;
    }

    if (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        can_ready = next_can_ready;
        stack_ready = next_stack_ready;
        xSemaphoreGive(view_mutex);
        request_render();
    }
}
}

namespace vt_view
{
void init()
{
    view_mutex = xSemaphoreCreateMutex();
    if (view_mutex == nullptr)
    {
        ESP_LOGE(TAG, "View mutex allocation failed");
        return;
    }

    const esp_err_t result = lcd_panel::init();
    panel_ready = (result == ESP_OK);
    if (!panel_ready)
    {
        ESP_LOGE(TAG, "LCD initialization failed: %s", esp_err_to_name(result));
        return;
    }

    const BaseType_t task_result = xTaskCreatePinnedToCore(
        render_task,
        "view_render",
        4096,
        nullptr,
        2,
        &render_task_handle,
        0);
    if (task_result != pdPASS)
    {
        render_task_handle = nullptr;
        panel_ready = false;
        ESP_LOGE(TAG, "View render task allocation failed");
        return;
    }

    ESP_LOGI(TAG, "VT diagnostics view initialized for %dx%d LCD", board::LCD_WIDTH, board::LCD_HEIGHT);
    render();
}

void set_status(const std::string &text)
{
    ESP_LOGI(TAG, "%s", text.c_str());
    update_status_and_render(text);
}

void set_bus_state(bool can_up, bool agisostack_up)
{
    ESP_LOGI(TAG, "CAN=%s AgIsoStack=%s", can_up ? "up" : "down", agisostack_up ? "up" : "disabled");
    update_bus_state_and_render(can_up, agisostack_up);
}

void set_can_diagnostics(const std::string &text)
{
    if (view_mutex == nullptr)
    {
        return;
    }

    if (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        if (can_diagnostics != text)
        {
            can_diagnostics = text;
            xSemaphoreGive(view_mutex);
            request_render();
            return;
        }
        xSemaphoreGive(view_mutex);
    }
}

void set_can_signal_diagnostics(const std::string &text)
{
    if (view_mutex == nullptr)
    {
        return;
    }

    if (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        if (can_signal_diagnostics != text)
        {
            can_signal_diagnostics = text;
            xSemaphoreGive(view_mutex);
            request_render();
            return;
        }
        xSemaphoreGive(view_mutex);
    }
}

void set_gnss_status(const std::string &text)
{
    if (view_mutex == nullptr)
    {
        return;
    }

    if (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        if (gnss_status == text)
        {
            xSemaphoreGive(view_mutex);
            return;
        }
        gnss_status = text;
        xSemaphoreGive(view_mutex);
        request_render();
    }
}

void set_section_control_status(const std::string &text)
{
    if (view_mutex == nullptr)
    {
        return;
    }

    if (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        section_control_status = text;
        xSemaphoreGive(view_mutex);
        request_render();
    }
}

void set_object_pool_download_progress(bool active, std::uint8_t percent)
{
    if (view_mutex == nullptr)
    {
        return;
    }

    percent = percent > 100 ? 100 : percent;
    if (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        if ((object_pool_download_active != active) || (object_pool_download_percent != percent))
        {
            object_pool_download_active = active;
            object_pool_download_percent = percent;
            xSemaphoreGive(view_mutex);
            request_render();
            return;
        }
        xSemaphoreGive(view_mutex);
    }
}

void on_machine_ecu_state(std::uint64_t name, std::uint8_t address, bool online)
{
    ESP_LOGI(TAG, "Machine ECU NAME=0x%016llx address=%u is %s",
             static_cast<unsigned long long>(name),
             static_cast<unsigned>(address),
             online ? "online" : "offline");

    char text[48];
    if (online)
    {
        std::snprintf(text, sizeof(text), "MACHINE ECU ONLINE SA %u", static_cast<unsigned>(address));
    }
    else
    {
        std::snprintf(text, sizeof(text), "MACHINE ECU OFFLINE");
    }
    update_status_and_render(text);
}

void on_object_pool_loaded(std::uint64_t client_name, std::size_t bytes)
{
    ESP_LOGI(TAG, "Object pool loaded: client=0x%016llx bytes=%u",
             static_cast<unsigned long long>(client_name),
             static_cast<unsigned>(bytes));
    update_status_and_render("WORKING SET RECEIVED");
}

void on_repaint_requested(std::uint64_t client_name)
{
    ESP_LOGD(TAG, "Repaint requested for client=0x%016llx", static_cast<unsigned long long>(client_name));
    update_status_and_render("WORKING SET REPAINT");
}

void on_active_mask_changed(std::uint64_t client_name, std::uint16_t previous_mask, std::uint16_t active_mask)
{
    ESP_LOGI(TAG, "Active mask client=0x%016llx %u -> %u",
             static_cast<unsigned long long>(client_name),
             previous_mask,
             active_mask);
    char text[48];
    std::snprintf(text, sizeof(text), "ACTIVE MASK %u", active_mask);
    update_status_and_render(text);
}

void on_soft_key_mask_changed(std::uint64_t client_name, std::uint16_t previous_mask, std::uint16_t active_mask)
{
    ESP_LOGI(TAG, "Soft key mask client=0x%016llx %u -> %u",
             static_cast<unsigned long long>(client_name),
             previous_mask,
             active_mask);
    char text[48];
    std::snprintf(text, sizeof(text), "SOFT KEY MASK %u", active_mask);
    update_status_and_render(text);
}

void on_focus_changed(std::uint64_t client_name, std::uint16_t object_id, bool input_open)
{
    ESP_LOGI(TAG, "Focus client=0x%016llx object=%u input=%s",
             static_cast<unsigned long long>(client_name),
             object_id,
             input_open ? "open" : "closed");
    char text[48];
    std::snprintf(text, sizeof(text), "FOCUS OBJECT %u", object_id);
    update_status_and_render(text);
}

void on_aux_n_input_device(std::uint64_t device_name, std::uint16_t model_identification_code)
{
    ESP_LOGI(TAG, "AUX-N input device NAME=0x%016llx model=%u",
             static_cast<unsigned long long>(device_name),
             model_identification_code);
    update_status_and_render("AUX-N INPUT FOUND");
}

void on_aux_n_assignment_result(std::uint64_t implement_name, std::uint16_t function_object_id, std::uint8_t error_code)
{
    ESP_LOGI(TAG, "AUX-N assignment response implement=0x%016llx function=%u error=0x%02x",
             static_cast<unsigned long long>(implement_name),
             function_object_id,
             error_code);
    char text[48];
    std::snprintf(text, sizeof(text), "AUX-N RESULT %u", error_code);
    update_status_and_render(text);
}

void on_touch(std::uint16_t x, std::uint16_t y, std::size_t count)
{
    ESP_LOGI(TAG, "Touch: x=%u y=%u points=%u", x, y, static_cast<unsigned>(count));
}

bool is_idle_menu_touch(std::uint16_t x, std::uint16_t y)
{
    if (view_mutex == nullptr)
    {
        return false;
    }

    bool idle_screen_visible = false;
    if (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        idle_screen_visible = panel_ready && !working_set_visible;
        xSemaphoreGive(view_mutex);
    }
    return idle_screen_visible &&
           (x >= MENU_BUTTON_X) && (x < MENU_BUTTON_X + MENU_BUTTON_WIDTH) &&
           (y >= MENU_BUTTON_Y) && (y < MENU_BUTTON_Y + MENU_BUTTON_HEIGHT);
}

void set_working_set_visible(bool visible)
{
    if (view_mutex == nullptr)
    {
        return;
    }

    if (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        working_set_visible = visible;
        xSemaphoreGive(view_mutex);
        if (!visible)
        {
            request_render();
        }
    }
}
}
