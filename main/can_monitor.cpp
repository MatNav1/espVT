#include "can_monitor.hpp"

#include "board_pins.hpp"
#include "board_support.hpp"
#include "isobus_terminal_server.hpp"
#include "lvgl_terminal_view.hpp"

#include "driver/twai.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

#include <atomic>
#include <array>
#include <cstdio>

#if ISOVT_ENABLE_AGISOSTACK
#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/hardware_integration/twai_plugin.hpp"
#include "isobus/isobus/can_callbacks.hpp"
#include "isobus/isobus/can_control_function.hpp"
#include "isobus/isobus/can_network_manager.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <memory>
#endif

namespace
{
constexpr char TAG[] = "can";

#if ISOVT_ENABLE_AGISOSTACK
twai_general_config_t general_config = TWAI_GENERAL_CONFIG_DEFAULT(board::CAN_TX, board::CAN_RX, TWAI_MODE_NORMAL);
twai_timing_config_t timing_config = TWAI_TIMING_CONFIG_250KBITS();
twai_filter_config_t filter_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

class ObservedTWAIPlugin final : public isobus::TWAIPlugin
{
public:
    using isobus::TWAIPlugin::TWAIPlugin;

    bool read_frame(isobus::CANMessageFrame &frame) override
    {
        if (!isobus::TWAIPlugin::read_frame(frame))
        {
            return false;
        }
        if (!peer_detected.exchange(true, std::memory_order_acq_rel))
        {
            ESP_LOGI(TAG, "CAN peer detected");
        }
        return true;
    }

private:
    std::atomic<bool> peer_detected{ false };
};

std::shared_ptr<ObservedTWAIPlugin> can_driver;
std::atomic<std::uint64_t> latest_machine_name = 0;
std::atomic<std::uint8_t> latest_machine_address = 0;
std::atomic<bool> latest_machine_online = false;
std::atomic<bool> remote_bus_activity_detected = false;
std::atomic<bool> transmit_enabled = false;
std::atomic<bool> ui_updates_enabled = false;
std::atomic<std::uint32_t> received_frame_count = 0;
std::atomic<std::uint32_t> machine_state_generation = 0;
std::atomic<std::uint32_t> tx_success_alert_count = 0;
std::atomic<std::uint32_t> tx_failed_alert_count = 0;
std::atomic<std::uint32_t> bus_error_alert_count = 0;
std::atomic<std::uint32_t> arbitration_lost_alert_count = 0;
volatile std::uint32_t can_rx_edge_count = 0;
std::array<std::atomic<std::uint64_t>, 256> machine_name_by_address{};
std::array<std::atomic<std::uint32_t>, 256> machine_last_rx_tick{};
std::array<std::atomic<bool>, 256> machine_online_by_address{};
bool twai_recovery_pending = false;
constexpr TickType_t MACHINE_INACTIVITY_TIMEOUT = pdMS_TO_TICKS(5000);

void IRAM_ATTR on_can_rx_edge(void *)
{
    ++can_rx_edge_count;
}

void begin_twai_recovery(const twai_status_info_t &status)
{
    ESP_LOGW(TAG,
             "TWAI bus-off: state=%u queued=%u tx_failed=%u txerr=%u rxerr=%u "
             "bus_errors=%u; starting controller recovery",
             static_cast<unsigned>(status.state),
             static_cast<unsigned>(status.msgs_to_tx),
             static_cast<unsigned>(status.tx_failed_count),
             static_cast<unsigned>(status.tx_error_counter),
             static_cast<unsigned>(status.rx_error_counter),
             static_cast<unsigned>(status.bus_error_count));

    twai_clear_transmit_queue();
    if (twai_initiate_recovery() == ESP_OK)
    {
        twai_recovery_pending = true;
    }
}

void service_twai_recovery()
{
    twai_status_info_t status = {};
    if (twai_get_status_info(&status) != ESP_OK)
    {
        return;
    }

    std::uint32_t alerts = 0;
    twai_read_alerts(&alerts, 0);
    if ((alerts & TWAI_ALERT_TX_SUCCESS) != 0)
    {
        tx_success_alert_count.fetch_add(1, std::memory_order_relaxed);
    }
    if ((alerts & TWAI_ALERT_TX_FAILED) != 0)
    {
        tx_failed_alert_count.fetch_add(1, std::memory_order_relaxed);
    }
    if ((alerts & TWAI_ALERT_BUS_ERROR) != 0)
    {
        bus_error_alert_count.fetch_add(1, std::memory_order_relaxed);
    }
    if ((alerts & TWAI_ALERT_ARB_LOST) != 0)
    {
        arbitration_lost_alert_count.fetch_add(1, std::memory_order_relaxed);
    }
    if ((status.state == TWAI_STATE_BUS_OFF) && !twai_recovery_pending)
    {
        begin_twai_recovery(status);
    }
    else if ((status.state == TWAI_STATE_STOPPED) && twai_recovery_pending)
    {
        if (twai_start() == ESP_OK)
        {
            twai_recovery_pending = false;
            if (transmit_enabled.load(std::memory_order_relaxed))
            {
                isobus::CANNetworkManager::CANNetwork.send_request_for_address_claim(0);
            }
            ESP_LOGI(TAG, "TWAI recovered and restarted");
        }
    }
}

void on_control_function_state_change(std::shared_ptr<isobus::ControlFunction> control_function,
                                      isobus::ControlFunctionState state)
{
    if ((control_function == nullptr) ||
        (control_function->get_type() == isobus::ControlFunction::Type::Internal) ||
        (control_function->get_NAME().get_function_code() ==
         static_cast<std::uint8_t>(isobus::NAME::Function::VirtualTerminal)))
    {
        return;
    }

    latest_machine_name.store(control_function->get_NAME().get_full_name(), std::memory_order_relaxed);
    latest_machine_address.store(control_function->get_address(), std::memory_order_relaxed);
    latest_machine_online.store(state == isobus::ControlFunctionState::Online, std::memory_order_relaxed);
    const std::uint8_t address = control_function->get_address();
    machine_name_by_address[address].store(control_function->get_NAME().get_full_name(), std::memory_order_relaxed);
    machine_online_by_address[address].store(state == isobus::ControlFunctionState::Online, std::memory_order_relaxed);
    if (state == isobus::ControlFunctionState::Online)
    {
        machine_last_rx_tick[address].store(xTaskGetTickCount(), std::memory_order_relaxed);
    }
    machine_state_generation.fetch_add(1, std::memory_order_release);
}

void machine_status_task(void *)
{
    std::uint32_t handled_generation = 0;
    TickType_t last_diagnostics_tick = 0;
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        service_twai_recovery();
        const TickType_t now = xTaskGetTickCount();
        for (std::size_t address = 0; address < machine_online_by_address.size(); ++address)
        {
            if (!machine_online_by_address[address].load(std::memory_order_relaxed))
            {
                continue;
            }
            const TickType_t last_rx = machine_last_rx_tick[address].load(std::memory_order_relaxed);
            if ((last_rx != 0) && ((now - last_rx) >= MACHINE_INACTIVITY_TIMEOUT))
            {
                machine_online_by_address[address].store(false, std::memory_order_relaxed);
                const std::uint64_t name = machine_name_by_address[address].load(std::memory_order_relaxed);
                ESP_LOGW(TAG, "Machine ECU SA %u inactive for 5 seconds; hiding its working set",
                         static_cast<unsigned>(address));
                isovt::on_machine_inactive(name, static_cast<std::uint8_t>(address));
                if (latest_machine_address.load(std::memory_order_relaxed) == address)
                {
                    latest_machine_name.store(name, std::memory_order_relaxed);
                    latest_machine_online.store(false, std::memory_order_relaxed);
                    machine_state_generation.fetch_add(1, std::memory_order_release);
                }
            }
        }
        if ((now - last_diagnostics_tick) >= pdMS_TO_TICKS(1000))
        {
            last_diagnostics_tick = now;
            twai_status_info_t status = {};
            if (twai_get_status_info(&status) == ESP_OK)
            {
                char text[96];
                std::snprintf(text,
                              sizeof(text),
                              "CAN RX:%lu RXQ:%lu TXQ:%lu OK:%lu FAIL:%lu S:%u",
                              static_cast<unsigned long>(received_frame_count.load(std::memory_order_relaxed)),
                              static_cast<unsigned long>(status.msgs_to_rx),
                              static_cast<unsigned long>(status.msgs_to_tx),
                              static_cast<unsigned long>(tx_success_alert_count.load(std::memory_order_relaxed)),
                              static_cast<unsigned long>(status.tx_failed_count),
                              static_cast<unsigned>(status.state));
                char signal_text[128];
                std::snprintf(signal_text,
                              sizeof(signal_text),
                              "PIN:%d EDGE:%lu TEC:%lu REC:%lu ERR:%lu ARB:%lu MISS:%lu",
                              gpio_get_level(board::CAN_RX),
                              static_cast<unsigned long>(can_rx_edge_count),
                              static_cast<unsigned long>(status.tx_error_counter),
                              static_cast<unsigned long>(status.rx_error_counter),
                              static_cast<unsigned long>(status.bus_error_count),
                              static_cast<unsigned long>(status.arb_lost_count),
                              static_cast<unsigned long>(status.rx_missed_count));
                if (ui_updates_enabled.load(std::memory_order_acquire))
                {
                    vt_view::set_can_diagnostics(text);
                    vt_view::set_can_signal_diagnostics(signal_text);
                }
                ESP_LOGI(TAG,
                         "%s | %s | alertFail=%lu alertBus=%lu alertArb=%lu",
                         text,
                         signal_text,
                         static_cast<unsigned long>(tx_failed_alert_count.load(std::memory_order_relaxed)),
                         static_cast<unsigned long>(bus_error_alert_count.load(std::memory_order_relaxed)),
                         static_cast<unsigned long>(arbitration_lost_alert_count.load(std::memory_order_relaxed)));
            }
        }
        const std::uint32_t generation = machine_state_generation.load(std::memory_order_acquire);
        if ((generation == handled_generation) ||
            !ui_updates_enabled.load(std::memory_order_acquire))
        {
            continue;
        }

        vt_view::on_machine_ecu_state(latest_machine_name.load(std::memory_order_relaxed),
                                      latest_machine_address.load(std::memory_order_relaxed),
                                      latest_machine_online.load(std::memory_order_relaxed));
        handled_generation = generation;
    }
}

esp_err_t start_agisostack_can_on_current_core()
{
    ESP_LOGI(TAG, "Before TWAI install: TX=%d RX=%d",
             gpio_get_level(board::CAN_TX),
             gpio_get_level(board::CAN_RX));
    general_config.tx_queue_len = 64;
    general_config.rx_queue_len = 256;
    general_config.alerts_enabled = TWAI_ALERT_TX_SUCCESS | TWAI_ALERT_TX_FAILED |
                                    TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                                    TWAI_ALERT_BUS_ERROR | TWAI_ALERT_ARB_LOST |
                                    TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_ERR_PASS |
                                    TWAI_ALERT_ABOVE_ERR_WARN | TWAI_ALERT_BELOW_ERR_WARN;
    can_driver = std::make_shared<ObservedTWAIPlugin>(&general_config, &timing_config, &filter_config);

    if (!isobus::CANHardwareInterface::set_number_of_can_channels(1, 256) ||
        !isobus::CANHardwareInterface::assign_can_channel_frame_handler(0, can_driver) ||
        !isobus::CANHardwareInterface::start() ||
        !can_driver->get_is_valid())
    {
        ESP_LOGE(TAG, "AgIsoStack++ could not start TWAI");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "After TWAI start: TX=%d RX=%d",
             gpio_get_level(board::CAN_TX),
             gpio_get_level(board::CAN_RX));
    ESP_RETURN_ON_ERROR(gpio_set_intr_type(board::CAN_RX, GPIO_INTR_ANYEDGE), TAG, "CAN RX edge interrupt failed");
    const esp_err_t isr_service_result = gpio_install_isr_service(0);
    if ((isr_service_result != ESP_OK) && (isr_service_result != ESP_ERR_INVALID_STATE))
    {
        ESP_LOGE(TAG, "GPIO ISR service failed: %s", esp_err_to_name(isr_service_result));
        return isr_service_result;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(board::CAN_RX, on_can_rx_edge, nullptr),
                        TAG,
                        "CAN RX edge handler failed");
    isobus::CANHardwareInterface::get_can_frame_received_event_dispatcher().add_listener(
        [](const isobus::CANMessageFrame &frame) {
            received_frame_count.fetch_add(1, std::memory_order_relaxed);
            remote_bus_activity_detected.store(true, std::memory_order_release);
            if (frame.isExtendedFrame)
            {
                const std::uint8_t source_address = static_cast<std::uint8_t>(frame.identifier & 0xFFU);
                machine_last_rx_tick[source_address].store(xTaskGetTickCount(), std::memory_order_relaxed);
            }
        });
    isobus::CANNetworkManager::CANNetwork.add_control_function_status_change_callback(on_control_function_state_change);
    if (xTaskCreate(machine_status_task, "can_status", 3072, nullptr, 3, nullptr) != pdPASS)
    {
        ESP_LOGE(TAG, "Could not create CAN recovery task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "AgIsoStack++ TWAI started at 250 kbit/s, TX=%d RX=%d", board::CAN_TX, board::CAN_RX);
    return ESP_OK;
}

#endif
}

namespace can_monitor
{
void enable_ui_updates()
{
    ui_updates_enabled.store(true, std::memory_order_release);
}

esp_err_t start()
{
    ESP_RETURN_ON_ERROR(board::enable_can_mode(), TAG, "CAN mode select failed");

#if ISOVT_ENABLE_AGISOSTACK
    if (isobus::CANHardwareInterface::is_running())
    {
        return ESP_OK;
    }

    // Install TWAI directly from app_main on CPU0. This matches the Waveshare
    // reference initialization and reliably takes GPIO19/20 from the native
    // USB peripheral after a cold power-on.
    return start_agisostack_can_on_current_core();
#else
    twai_general_config_t general_config = TWAI_GENERAL_CONFIG_DEFAULT(board::CAN_TX, board::CAN_RX, TWAI_MODE_NORMAL);
    general_config.tx_queue_len = 20;
    general_config.rx_queue_len = 32;

    twai_timing_config_t timing_config = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t filter_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t result = twai_driver_install(&general_config, &timing_config, &filter_config);
    if ((result != ESP_OK) && (result != ESP_ERR_INVALID_STATE))
    {
        return result;
    }

    result = twai_start();
    if ((result != ESP_OK) && (result != ESP_ERR_INVALID_STATE))
    {
        return result;
    }

    ESP_LOGI(TAG, "TWAI started at 250 kbit/s, TX=%d RX=%d", board::CAN_TX, board::CAN_RX);
    return ESP_OK;
#endif
}

bool bus_activity_detected()
{
#if ISOVT_ENABLE_AGISOSTACK
    return remote_bus_activity_detected.load(std::memory_order_acquire);
#else
    return true;
#endif
}

bool request_address_claims()
{
#if ISOVT_ENABLE_AGISOSTACK
    if (!isobus::CANHardwareInterface::is_running())
    {
        return false;
    }

    transmit_enabled.store(true, std::memory_order_release);
    const bool requested = isobus::CANNetworkManager::CANNetwork.send_request_for_address_claim(0);
    ESP_LOGI(TAG, "ISOBUS address claim request: %s", requested ? "sent" : "not sent");
    if (!requested)
    {
        twai_status_info_t status = {};
        if (twai_get_status_info(&status) == ESP_OK)
        {
            ESP_LOGW(TAG,
                     "TWAI request failed: state=%u queued=%u tx_failed=%u txerr=%u rxerr=%u",
                     static_cast<unsigned>(status.state),
                     static_cast<unsigned>(status.msgs_to_tx),
                     static_cast<unsigned>(status.tx_failed_count),
                     static_cast<unsigned>(status.tx_error_counter),
                     static_cast<unsigned>(status.rx_error_counter));
        }
    }
    return requested;
#else
    return false;
#endif
}
}
