#include <Arduino.h>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "driver/twai.h"
#include "esp_log.h"

#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/hardware_integration/twai_plugin.hpp"
#include "isobus/isobus/can_internal_control_function.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_partnered_control_function.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/isobus/isobus_virtual_terminal_client.hpp"
#include "isobus/utility/iop_file_interface.hpp"

#include "generated/aux_n_pool_data.hpp"
#include "generated/device_config.hpp"

// Wiring: GPIO22 -> transceiver CAN TX, GPIO21 <- transceiver CAN RX.
static constexpr gpio_num_t CAN_TX_PIN = GPIO_NUM_22;
static constexpr gpio_num_t CAN_RX_PIN = GPIO_NUM_21;
static constexpr uint32_t DEBOUNCE_MS = 30;
static constexpr uint32_t FORCE_RESYNC_MS = 1000;
static constexpr uint32_t CAN_RECOVERY_CHECK_MS = 100;

class SerialLogger : public isobus::CANStackLogger {
public:
    void sink_CAN_stack_log(LoggingLevel, const std::string &text) override {
        Serial.println(text.c_str());
    }
};

struct ButtonState {
    bool rawPressed = false;
    bool stablePressed = false;
    uint32_t changedAt = 0;
    uint16_t transitionCount = 0;
};

static SerialLogger logger;
static std::array<ButtonState, NUM_AUX_BUTTONS> buttons;
static std::shared_ptr<isobus::InternalControlFunction> internalECU;
static std::shared_ptr<isobus::VirtualTerminalClient> vtClient;
static uint32_t lastForceResync = 0;
static uint32_t lastCanRecoveryCheck = 0;
static bool canRecoveryPending = false;

static uint32_t deviceIdentityNumber() {
    if (!AUX_USE_HARDWARE_IDENTITY) {
        return AUX_IDENTITY_NUMBER;
    }
    uint32_t identity = static_cast<uint32_t>(ESP.getEfuseMac() & 0x1FFFFFULL);
    return identity == 0 ? 1 : identity;
}

static void serviceCanRecovery() {
    const uint32_t now = millis();
    if (now - lastCanRecoveryCheck < CAN_RECOVERY_CHECK_MS) {
        return;
    }
    lastCanRecoveryCheck = now;

    twai_status_info_t status = {};
    if (twai_get_status_info(&status) != ESP_OK) {
        return;
    }
    if ((status.state == TWAI_STATE_BUS_OFF) && !canRecoveryPending) {
        Serial.printf("[CAN] bus-off (txerr=%u rxerr=%u), starting recovery\n",
                      status.tx_error_counter, status.rx_error_counter);
        twai_clear_transmit_queue();
        if (twai_initiate_recovery() == ESP_OK) {
            canRecoveryPending = true;
        }
    } else if ((status.state == TWAI_STATE_STOPPED) && canRecoveryPending) {
        if (twai_start() == ESP_OK) {
            canRecoveryPending = false;
            lastForceResync = 0;
            isobus::CANNetworkManager::CANNetwork.send_request_for_address_claim(0);
            Serial.println("[CAN] recovered and restarted");
        }
    }
}

static void setupIsobus() {
    esp_log_level_set("gpio", ESP_LOG_WARN);
    esp_log_level_set("twai", ESP_LOG_WARN);
    isobus::CANStackLogger::set_can_stack_logger_sink(&logger);
    isobus::CANStackLogger::set_log_level(isobus::CANStackLogger::LoggingLevel::Info);

    twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
    twai_timing_config_t timing = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    auto canDriver = std::make_shared<isobus::TWAIPlugin>(&general, &timing, &filter);

    isobus::CANHardwareInterface::set_number_of_can_channels(1);
    isobus::CANHardwareInterface::assign_can_channel_frame_handler(0, canDriver);
    if (!isobus::CANHardwareInterface::start() || !canDriver->get_is_valid()) {
        Serial.println("[AUX-N] ERROR: cannot start CAN/TWAI");
    }
    twai_reconfigure_alerts(TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                                TWAI_ALERT_BUS_ERROR | TWAI_ALERT_ERR_PASS,
                            nullptr);

    isobus::NAME name(0);
    name.set_arbitrary_address_capable(true);
    name.set_industry_group(2);
    name.set_device_class(0);
    name.set_function_code(129);
    name.set_identity_number(deviceIdentityNumber());
    name.set_ecu_instance(0);
    name.set_function_instance(0);
    name.set_device_class_instance(0);
    name.set_manufacturer_code(AUX_MANUFACTURER_CODE); // Bench code by default; replace for commercial use.

    const isobus::NAMEFilter vtFilter(
        isobus::NAME::NAMEParameters::FunctionCode,
        static_cast<uint8_t>(isobus::NAME::Function::VirtualTerminal));
    const std::vector<isobus::NAMEFilter> vtFilters = {vtFilter};
    internalECU = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(name, 0);
    auto partnerVT = isobus::CANNetworkManager::CANNetwork.create_partnered_control_function(0, vtFilters);

    vtClient = std::make_shared<isobus::VirtualTerminalClient>(partnerVT, internalECU);
    const std::string version = isobus::IOPFileInterface::hash_object_pool_to_version(
        AUX_N_POOL_DATA, AUX_N_POOL_SIZE);
    vtClient->set_object_pool(0, AUX_N_POOL_DATA, AUX_N_POOL_SIZE, version);
    vtClient->set_auxiliary_input_model_identification_code(AUX_MODEL_IDENTIFICATION_CODE);
    for (uint8_t i = 0; i < NUM_AUX_BUTTONS; ++i) {
        vtClient->add_auxiliary_input_object_id(AUX_BUTTON_IDS[i]);
    }
    vtClient->initialize(true);
}

static void scanButtons(bool force) {
    const uint32_t now = millis();
    for (uint8_t i = 0; i < NUM_AUX_BUTTONS; ++i) {
        ButtonState &state = buttons[i];
        const bool pressed = digitalRead(BUTTON_PINS[i]) == LOW;

        if (pressed != state.rawPressed) {
            state.rawPressed = pressed;
            state.changedAt = now;
        }
        if ((state.stablePressed != state.rawPressed) &&
            (now - state.changedAt >= DEBOUNCE_MS)) {
            state.stablePressed = state.rawPressed;
            ++state.transitionCount;
            vtClient->update_auxiliary_input(AUX_BUTTON_IDS[i],
                                             state.stablePressed ? 1 : 0,
                                             state.transitionCount);
            Serial.printf("[BTN] B%u GPIO%u = %s\n", i + 1, BUTTON_PINS[i],
                          state.stablePressed ? "ON" : "OFF");
        } else if (force) {
            vtClient->update_auxiliary_input(AUX_BUTTON_IDS[i],
                                             state.stablePressed ? 1 : 0,
                                             state.transitionCount);
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.printf("\nAUX-N buttons: %u inputs\n", NUM_AUX_BUTTONS);
    Serial.printf("  ISO NAME identity: %u, manufacturer: %u, model: %u\n",
                  deviceIdentityNumber(), AUX_MANUFACTURER_CODE, AUX_MODEL_IDENTIFICATION_CODE);
    for (uint8_t i = 0; i < NUM_AUX_BUTTONS; ++i) {
        pinMode(BUTTON_PINS[i], INPUT_PULLUP);
        const bool pressed = digitalRead(BUTTON_PINS[i]) == LOW;
        buttons[i].rawPressed = pressed;
        buttons[i].stablePressed = pressed;
        buttons[i].changedAt = millis();
        Serial.printf("  B%u: GPIO%u (active LOW)\n", i + 1, BUTTON_PINS[i]);
    }
    setupIsobus();
    Serial.println("[AUX-N] started; waiting for VT");
}

void loop() {
    serviceCanRecovery();
    isobus::CANHardwareInterface::update();
    vtClient->update();

    const uint32_t now = millis();
    const bool force = now - lastForceResync >= FORCE_RESYNC_MS;
    if (force) {
        lastForceResync = now;
    }
    scanButtons(force);
    delay(1);
}
