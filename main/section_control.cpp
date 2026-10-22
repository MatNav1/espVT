#include "section_control.hpp"

#include "gnss_um982.hpp"
#include "lvgl_terminal_view.hpp"

#include <cstdio>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace
{
constexpr std::uint32_t POSITION_MAX_AGE_MS = 1000;
constexpr std::uint32_t RTCM_MAX_AGE_MS = 5000;
constexpr std::uint32_t MONITOR_INTERVAL_MS = 200;

#if CONFIG_ISOVT_GNSS_SOURCE_UM982 || CONFIG_ISOVT_GNSS_SOURCE_RS232
constexpr std::uint8_t CONFIGURED_SECTION_COUNT = static_cast<std::uint8_t>(CONFIG_ISOVT_SECTION_CONTROL_SECTION_COUNT);
#else
constexpr std::uint8_t CONFIGURED_SECTION_COUNT = 0;
#endif

SemaphoreHandle_t state_mutex = nullptr;
section_control::Snapshot state = {};
bool monitor_started = false;

std::int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

bool take_state_lock()
{
    return (state_mutex != nullptr) && (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
}

section_control::Snapshot assess_gate()
{
    section_control::Snapshot result = {};
    result.configured_sections = CONFIGURED_SECTION_COUNT;
    if (!gnss_um982::is_enabled())
    {
        result.gate_state = section_control::GateState::Disabled;
        return result;
    }

    const auto gnss = gnss_um982::get_snapshot();
    if (!gnss.receiver_configured)
    {
        result.gate_state = section_control::GateState::PinsRequired;
        return result;
    }
    if (!gnss.position_valid)
    {
        result.gate_state = section_control::GateState::PositionUnavailable;
        return result;
    }

    const std::int64_t timestamp = now_ms();
    if ((timestamp - gnss.last_position_ms) > POSITION_MAX_AGE_MS)
    {
        result.gate_state = section_control::GateState::PositionStale;
        return result;
    }

    bool sufficient_fix = (gnss.fix_quality == gnss_um982::FixQuality::RtkFixed);
#if CONFIG_ISOVT_SECTION_CONTROL_ALLOW_RTK_FLOAT
    sufficient_fix = sufficient_fix || (gnss.fix_quality == gnss_um982::FixQuality::RtkFloat);
#endif
    if (!sufficient_fix)
    {
        result.gate_state = section_control::GateState::MinimumFixNotMet;
        return result;
    }
    if (gnss.ntrip_enabled && (!gnss.ntrip_connected || ((timestamp - gnss.last_rtcm_ms) > RTCM_MAX_AGE_MS)))
    {
        result.gate_state = section_control::GateState::CorrectionsStale;
        return result;
    }

    result.gate_state = section_control::GateState::Ready;
    return result;
}

bool state_changed(const section_control::Snapshot &left, const section_control::Snapshot &right)
{
    return (left.gate_state != right.gate_state) ||
           (left.configured_sections != right.configured_sections) ||
           (left.output_locked != right.output_locked);
}

void publish_status(const section_control::Snapshot &snapshot)
{
    char text[72] = {};
    if (snapshot.gate_state == section_control::GateState::Ready)
    {
        std::snprintf(text, sizeof(text), "SECTION GEO: READY %u SECTIONS OUTPUT LOCKED", snapshot.configured_sections);
    }
    else
    {
        std::snprintf(text, sizeof(text), "SECTION GEO: %s", section_control::gate_state_text(snapshot.gate_state));
    }
    vt_view::set_section_control_status(text);
}

void section_control_task(void *)
{
    while (true)
    {
        const auto next = assess_gate();
        bool changed = false;
        if (take_state_lock())
        {
            changed = state_changed(state, next);
            state = next;
            xSemaphoreGive(state_mutex);
        }
        if (changed)
        {
            publish_status(next);
        }
        vTaskDelay(pdMS_TO_TICKS(MONITOR_INTERVAL_MS));
    }
}
}

namespace section_control
{
bool start()
{
    if (monitor_started)
    {
        return true;
    }

    state_mutex = xSemaphoreCreateMutex();
    if (state_mutex == nullptr)
    {
        return false;
    }

    const auto initial = assess_gate();
    if (take_state_lock())
    {
        state = initial;
        xSemaphoreGive(state_mutex);
    }
    publish_status(initial);

    if (xTaskCreate(section_control_task, "section_gate", 3072, nullptr, 4, nullptr) != pdPASS)
    {
        return false;
    }
    monitor_started = true;
    return true;
}

Snapshot get_snapshot()
{
    Snapshot snapshot = {};
    if (take_state_lock())
    {
        snapshot = state;
        xSemaphoreGive(state_mutex);
    }
    return snapshot;
}

bool is_ready()
{
    return get_snapshot().gate_state == GateState::Ready;
}

const char *gate_state_text(GateState gate_state)
{
    switch (gate_state)
    {
        case GateState::PinsRequired:
            return "GNSS UART REQUIRED";
        case GateState::PositionUnavailable:
            return "NO POSITION";
        case GateState::PositionStale:
            return "POSITION STALE";
        case GateState::MinimumFixNotMet:
            return "MINIMUM RTK NOT MET";
        case GateState::CorrectionsStale:
            return "NTRIP CORRECTIONS STALE";
        case GateState::Ready:
            return "READY";
        default:
            return "GNSS INPUT OFF";
    }
}
}
