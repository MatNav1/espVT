#include "aux_n_manager.hpp"

#include "lvgl_terminal_view.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#if ISOVT_ENABLE_AGISOSTACK
#include "isobus/isobus/can_callbacks.hpp"
#include "isobus/isobus/can_general_parameter_group_numbers.hpp"
#include "isobus/isobus/can_identifier.hpp"
#include "isobus/isobus/can_message.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/isobus_virtual_terminal_objects.hpp"
#include "isobus/isobus/isobus_virtual_terminal_server_managed_working_set.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>
#endif

namespace
{
constexpr char TAG[] = "aux_n";

#if ISOVT_ENABLE_AGISOSTACK
constexpr std::uint8_t AUXILIARY_INPUT_MAINTENANCE = 0x23;
constexpr std::uint8_t PREFERRED_ASSIGNMENT = 0x22;
constexpr std::uint8_t AUXILIARY_ASSIGNMENT_TYPE_TWO = 0x24;
constexpr std::uint8_t AUXILIARY_INPUT_STATUS_ENABLE = 0x25;
constexpr std::uint8_t AUXILIARY_INPUT_STATUS_TYPE_TWO = 0x26;
constexpr std::uint8_t AUXILIARY_ASSIGNMENT_TYPE_ONE = 0x20;
constexpr std::uint8_t REMOVE_ASSIGNMENT = 0x1F;
constexpr std::uint16_t NULL_OBJECT_ID = 0xFFFF;
constexpr std::uint64_t NULL_DEVICE_NAME = UINT64_MAX;
// AUX-N maintenance frames can be delayed while an object pool is transferred
// or the CAN bus is busy. A 300 ms timeout made the input device repeatedly
// disappear and reappear, forcing a full AUX catalog rebuild on subsequent
// screen repaints. Keep enough margin to ride through short bus stalls.
constexpr std::int64_t AUXILIARY_INPUT_TIMEOUT_US = 3000000;
constexpr std::int64_t AUXILIARY_LEARN_TIMEOUT_US = 30000000;
constexpr char AUX_NVS_NAMESPACE[] = "isovt_aux";
constexpr char AUX_NVS_ASSIGNMENTS[] = "assign";

struct StoredAssignment
{
    std::uint64_t implement_name;
    std::uint64_t device_name;
    std::uint16_t function_object_id;
    std::uint16_t input_object_id;
    std::uint8_t protocol;
    std::uint8_t preferred;
};

class Manager
{
public:
    bool start(const std::shared_ptr<isobus::InternalControlFunction> &terminal)
    {
        if (!terminal)
        {
            return false;
        }
        if (started)
        {
            return true;
        }

        terminal_control_function = terminal;
        load_assignments();
        isobus::CANNetworkManager::CANNetwork.add_any_control_function_parameter_group_number_callback(
            static_cast<std::uint32_t>(isobus::CANLibParameterGroupNumber::ECUtoVirtualTerminal),
            &Manager::on_message,
            this);
        started = true;
        return true;
    }

    std::vector<aux_n::InputDevice> get_input_devices()
    {
        std::lock_guard<std::mutex> lock(devices_mutex);
        const std::int64_t now = esp_timer_get_time();
        input_devices.erase(std::remove_if(input_devices.begin(),
                                           input_devices.end(),
                                           [now](const aux_n::InputDevice &entry) {
                                               return (now - entry.last_seen_us) > AUXILIARY_INPUT_TIMEOUT_US;
                                           }),
                            input_devices.end());
        return input_devices;
    }

    void refresh_catalog(
        const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &active_working_set,
        const std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> &working_sets)
    {
        const auto devices = get_input_devices();
        std::uint64_t signature = legacy_type1_enabled ? 0xA0A0A0A0ULL : 0x50505050ULL;
        const auto mix_signature = [&signature](std::uint64_t value) {
            signature ^= value + 0x9E3779B97F4A7C15ULL + (signature << 6U) + (signature >> 2U);
        };
        if (active_working_set && active_working_set->get_control_function())
        {
            mix_signature(active_working_set->get_control_function()->get_NAME().get_full_name());
            mix_signature(active_working_set->get_object_tree().size());
        }
        for (const auto &working_set : working_sets)
        {
            if (working_set && working_set->get_control_function())
            {
                mix_signature(working_set->get_control_function()->get_NAME().get_full_name());
                mix_signature(working_set->get_object_tree().size());
            }
        }
        for (const auto &device : devices)
        {
            mix_signature(device.name);
            mix_signature(device.model_identification_code);
        }
        {
            std::lock_guard<std::mutex> lock(catalog_mutex);
            if (signature == catalog_signature)
            {
                return;
            }
        }
        std::vector<aux_n::FunctionDescriptor> functions;
        std::vector<aux_n::InputDescriptor> inputs;
        std::map<std::uint64_t, std::weak_ptr<isobus::ControlFunction>> controls;
        const auto scan = [&](const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set,
                              bool scan_functions) {
            if (!working_set || !working_set->get_control_function())
            {
                return;
            }
            const auto control = working_set->get_control_function();
            const std::uint64_t name = control->get_NAME().get_full_name();
            controls[name] = control;
            const auto online_device = std::find_if(devices.begin(), devices.end(), [name](const aux_n::InputDevice &entry) {
                return entry.name == name;
            });
            const std::uint16_t model = online_device == devices.end() ? 0U : online_device->model_identification_code;

            for (const auto &entry : working_set->get_object_tree())
            {
                const auto &object = entry.second;
                if (!object)
                {
                    continue;
                }
                if (!legacy_type1_enabled && scan_functions &&
                    (object->get_object_type() == isobus::VirtualTerminalObjectType::AuxiliaryFunctionType2))
                {
                    const auto function = std::static_pointer_cast<isobus::AuxiliaryFunctionType2>(object);
                    functions.push_back({ aux_n::Protocol::Type2,
                                          name,
                                          entry.first,
                                          static_cast<std::uint8_t>(function->get_function_type()),
                                          function->get_function_attribute(isobus::AuxiliaryFunctionType2::FunctionAttribute::CriticalControl),
                                          function->get_function_attribute(isobus::AuxiliaryFunctionType2::FunctionAttribute::AssignmentRestriction),
                                          function->get_function_attribute(isobus::AuxiliaryFunctionType2::FunctionAttribute::SingleAssignment) });
                }
                else if (!legacy_type1_enabled &&
                         (object->get_object_type() == isobus::VirtualTerminalObjectType::AuxiliaryInputType2))
                {
                    const auto input = std::static_pointer_cast<isobus::AuxiliaryInputType2>(object);
                    inputs.push_back({ aux_n::Protocol::Type2,
                                       name,
                                       model,
                                       entry.first,
                                       0xFF,
                                       static_cast<std::uint8_t>(input->get_function_type()),
                                       input->get_function_attribute(isobus::AuxiliaryInputType2::FunctionAttribute::CriticalControl),
                                       input->get_function_attribute(isobus::AuxiliaryInputType2::FunctionAttribute::SingleAssignment),
                                       online_device != devices.end() });
                }
                else if (legacy_type1_enabled && scan_functions &&
                         (object->get_object_type() == isobus::VirtualTerminalObjectType::AuxiliaryFunctionType1))
                {
                    const auto function = std::static_pointer_cast<isobus::AuxiliaryFunctionType1>(object);
                    functions.push_back({ aux_n::Protocol::LegacyType1,
                                          name,
                                          entry.first,
                                          static_cast<std::uint8_t>(function->get_function_type()) });
                }
                else if (legacy_type1_enabled &&
                         (object->get_object_type() == isobus::VirtualTerminalObjectType::AuxiliaryInputType1))
                {
                    const auto input = std::static_pointer_cast<isobus::AuxiliaryInputType1>(object);
                    inputs.push_back({ aux_n::Protocol::LegacyType1,
                                       name,
                                       model,
                                       entry.first,
                                       input->get_input_id(),
                                       static_cast<std::uint8_t>(input->get_function_type()),
                                       false,
                                       false,
                                       true });
                }
            }
        };

        scan(active_working_set, true);
        for (const auto &working_set : working_sets)
        {
            if (working_set != active_working_set)
            {
                scan(working_set, false);
            }
        }

        std::vector<std::tuple<std::shared_ptr<isobus::ControlFunction>,
                               std::shared_ptr<isobus::ControlFunction>, aux_n::Assignment, std::uint8_t>> restore;
        {
            std::lock_guard<std::mutex> lock(catalog_mutex);
            catalog.functions = std::move(functions);
            catalog.inputs = std::move(inputs);
            control_functions = std::move(controls);
            if (std::none_of(catalog.functions.begin(), catalog.functions.end(), [this](const aux_n::FunctionDescriptor &function) {
                    return (function.protocol == catalog.selected_protocol) &&
                           (function.implement_name == catalog.selected_implement_name) &&
                           (function.object_id == catalog.selected_function_object_id);
                }))
            {
                catalog.selected_implement_name = 0;
                catalog.selected_function_object_id = NULL_OBJECT_ID;
            }
            catalog.legacy_type1_enabled = legacy_type1_enabled;
            catalog_signature = signature;

            for (const auto &assignment : catalog.assignments)
            {
                const auto key = std::make_pair(assignment.implement_name, assignment.function_object_id);
                const auto function = std::find_if(catalog.functions.begin(), catalog.functions.end(), [&](const auto &entry) {
                    return (entry.protocol == assignment.protocol) && (entry.implement_name == assignment.implement_name) &&
                           (entry.object_id == assignment.function_object_id);
                });
                const auto input = std::find_if(catalog.inputs.begin(), catalog.inputs.end(), [&](const auto &entry) {
                    return (entry.protocol == assignment.protocol) && (entry.device_name == assignment.device_name) &&
                           (entry.object_id == assignment.input_object_id) && entry.online;
                });
                auto implement = control_functions[assignment.implement_name].lock();
                auto device = control_functions[assignment.device_name].lock();
                if ((assignment.protocol == aux_n::Protocol::Type2) && (function != catalog.functions.end()) &&
                    (input != catalog.inputs.end()) && implement && device && restored_assignments.insert(key).second)
                {
                    restore.emplace_back(implement, device, assignment, function->function_type);
                }
            }
        }
        for (const auto &[implement, device, assignment, function_type] : restore)
        {
            send_status_enable(device, assignment.input_object_id, true);
            send_assignment(implement, assignment.device_name, function_type, assignment.input_object_id,
                            assignment.function_object_id, assignment.preferred);
        }
    }

    aux_n::Catalog get_catalog()
    {
        std::lock_guard<std::mutex> lock(catalog_mutex);
        if (catalog.learning && ((esp_timer_get_time() - learning_started_us) > AUXILIARY_LEARN_TIMEOUT_US))
        {
            catalog.learning = false;
        }
        return catalog;
    }

    bool select_function(aux_n::Protocol protocol, std::uint64_t implement_name, std::uint16_t object_id)
    {
        std::lock_guard<std::mutex> lock(catalog_mutex);
        const auto found = std::find_if(catalog.functions.begin(), catalog.functions.end(), [&](const aux_n::FunctionDescriptor &function) {
            return (function.protocol == protocol) && (function.implement_name == implement_name) && (function.object_id == object_id);
        });
        if (found == catalog.functions.end())
        {
            return false;
        }
        catalog.selected_protocol = protocol;
        catalog.selected_implement_name = implement_name;
        catalog.selected_function_object_id = object_id;
        return true;
    }

    bool assign_selected(aux_n::Protocol protocol, std::uint64_t device_name, std::uint16_t input_object_id)
    {
        aux_n::FunctionDescriptor function;
        aux_n::InputDescriptor input;
        std::shared_ptr<isobus::ControlFunction> implement;
        std::shared_ptr<isobus::ControlFunction> input_device;
        {
            std::lock_guard<std::mutex> lock(catalog_mutex);
            const auto selected = std::find_if(catalog.functions.begin(), catalog.functions.end(), [&](const aux_n::FunctionDescriptor &candidate) {
                return (candidate.protocol == catalog.selected_protocol) &&
                       (candidate.implement_name == catalog.selected_implement_name) &&
                       (candidate.object_id == catalog.selected_function_object_id);
            });
            const auto selected_input = std::find_if(catalog.inputs.begin(), catalog.inputs.end(), [&](const aux_n::InputDescriptor &candidate) {
                return (candidate.protocol == protocol) && (candidate.device_name == device_name) &&
                       (candidate.object_id == input_object_id);
            });
            if ((selected == catalog.functions.end()) || (selected_input == catalog.inputs.end()) ||
                (selected->protocol != protocol) || (selected->function_type != selected_input->function_type) ||
                (selected->critical && !selected_input->critical))
            {
                return false;
            }
            function = *selected;
            input = *selected_input;
            implement = control_functions[function.implement_name].lock();
            input_device = control_functions[input.device_name].lock();
        }
        if (!implement || !input_device)
        {
            return false;
        }
        if (protocol == aux_n::Protocol::Type2)
        {
            send_status_enable(input_device, input.object_id, true);
            const bool sent = send_assignment(implement,
                                              input.device_name,
                                              function.function_type,
                                              input.object_id,
                                              function.object_id,
                                              true);
            if (sent)
            {
                std::lock_guard<std::mutex> lock(catalog_mutex);
                remember_pending(function, input, true);
                catalog.learning = false;
            }
            return sent;
        }
        const bool sent = legacy_type1_enabled && send_legacy_assignment(implement,
                                                                          input_device->get_address(),
                                                                          input.input_number,
                                                                          function.object_id);
        if (sent)
        {
            std::lock_guard<std::mutex> lock(catalog_mutex);
            remember_pending(function, input, true);
        }
        return sent;
    }

    bool begin_learning()
    {
        std::vector<std::pair<std::shared_ptr<isobus::ControlFunction>, std::uint16_t>> enabled_inputs;
        {
            std::lock_guard<std::mutex> lock(catalog_mutex);
            const auto selected = selected_function_locked();
            if ((selected == catalog.functions.end()) || (selected->protocol != aux_n::Protocol::Type2))
            {
                return false;
            }
            for (const auto &input : catalog.inputs)
            {
                if (is_compatible(*selected, input) && input.online)
                {
                    if (auto control = control_functions[input.device_name].lock())
                    {
                        enabled_inputs.emplace_back(control, input.object_id);
                    }
                }
            }
            if (enabled_inputs.empty())
            {
                return false;
            }
            catalog.learning = true;
            learning_started_us = esp_timer_get_time();
        }
        for (const auto &input : enabled_inputs)
        {
            send_status_enable(input.first, input.second, true);
        }
        return true;
    }

    void cancel_learning()
    {
        std::lock_guard<std::mutex> lock(catalog_mutex);
        catalog.learning = false;
    }

    bool unassign_selected()
    {
        std::shared_ptr<isobus::ControlFunction> implement;
        std::uint16_t function_id = NULL_OBJECT_ID;
        {
            std::lock_guard<std::mutex> lock(catalog_mutex);
            const auto selected = selected_function_locked();
            if (selected == catalog.functions.end())
            {
                return false;
            }
            implement = control_functions[selected->implement_name].lock();
            function_id = selected->object_id;
        }
        if (!implement || !send_assignment(implement, NULL_DEVICE_NAME, REMOVE_ASSIGNMENT,
                                            NULL_OBJECT_ID, function_id, true))
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(catalog_mutex);
        pending_removals.insert({ implement->get_NAME().get_full_name(), function_id });
        return true;
    }

    bool unassign_all_tracked(const std::shared_ptr<isobus::ControlFunction> &implement, bool preferred)
    {
        if (!implement || !send_assignment(implement, NULL_DEVICE_NAME, REMOVE_ASSIGNMENT,
                                            NULL_OBJECT_ID, NULL_OBJECT_ID, preferred))
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(catalog_mutex);
        pending_removals.insert({ implement->get_NAME().get_full_name(), NULL_OBJECT_ID });
        return true;
    }

    void set_legacy_type1_enabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(catalog_mutex);
        legacy_type1_enabled = enabled;
        catalog.legacy_type1_enabled = enabled;
    }

    bool send_assignment(const std::shared_ptr<isobus::ControlFunction> &implement,
                         std::uint64_t input_device_name,
                         std::uint8_t function_type,
                         std::uint16_t input_object_id,
                         std::uint16_t function_object_id,
                         bool store_as_preferred)
    {
        const auto terminal = terminal_control_function.lock();
        if (!terminal || !implement || ((function_type > static_cast<std::uint8_t>(aux_n::FunctionType::BidirectionalEncoder)) &&
                                       (function_type != REMOVE_ASSIGNMENT)))
        {
            return false;
        }

        std::array<std::uint8_t, 14> buffer = {};
        buffer.fill(0xFF);
        buffer[0] = AUXILIARY_ASSIGNMENT_TYPE_TWO;
        for (std::uint8_t index = 0; index < 8; ++index)
        {
            buffer[1 + index] = static_cast<std::uint8_t>(input_device_name >> (index * 8U));
        }
        buffer[9] = static_cast<std::uint8_t>(function_type & 0x1FU);
        if (store_as_preferred)
        {
            buffer[9] |= 0x80U;
        }
        buffer[10] = static_cast<std::uint8_t>(input_object_id);
        buffer[11] = static_cast<std::uint8_t>(input_object_id >> 8U);
        buffer[12] = static_cast<std::uint8_t>(function_object_id);
        buffer[13] = static_cast<std::uint8_t>(function_object_id >> 8U);

        return isobus::CANNetworkManager::CANNetwork.send_can_message(
            static_cast<std::uint32_t>(isobus::CANLibParameterGroupNumber::VirtualTerminalToECU),
            buffer.data(),
            buffer.size(),
            terminal,
            implement,
            isobus::CANIdentifier::CANPriority::Priority3);
    }

    bool send_status_enable(const std::shared_ptr<isobus::ControlFunction> &input_device,
                            std::uint16_t input_object_id,
                            bool enable)
    {
        const auto terminal = terminal_control_function.lock();
        if (!terminal || !input_device)
        {
            return false;
        }
        std::array<std::uint8_t, 8> buffer = { AUXILIARY_INPUT_STATUS_ENABLE,
                                               static_cast<std::uint8_t>(input_object_id),
                                               static_cast<std::uint8_t>(input_object_id >> 8U),
                                               static_cast<std::uint8_t>(enable ? 1U : 0U),
                                               0xFF, 0xFF, 0xFF, 0xFF };
        return isobus::CANNetworkManager::CANNetwork.send_can_message(
            static_cast<std::uint32_t>(isobus::CANLibParameterGroupNumber::VirtualTerminalToECU),
            buffer.data(), buffer.size(), terminal, input_device,
            isobus::CANIdentifier::CANPriority::Priority3);
    }

    bool send_legacy_assignment(const std::shared_ptr<isobus::ControlFunction> &implement,
                                std::uint8_t input_source_address,
                                std::uint8_t input_number,
                                std::uint16_t function_object_id)
    {
        const auto terminal = terminal_control_function.lock();
        if (!terminal || !implement)
        {
            return false;
        }
        std::array<std::uint8_t, 8> buffer = { AUXILIARY_ASSIGNMENT_TYPE_ONE,
                                               input_source_address,
                                               input_number,
                                               static_cast<std::uint8_t>(function_object_id),
                                               static_cast<std::uint8_t>(function_object_id >> 8U),
                                               0xFF, 0xFF, 0xFF };
        return isobus::CANNetworkManager::CANNetwork.send_can_message(
            static_cast<std::uint32_t>(isobus::CANLibParameterGroupNumber::VirtualTerminalToECU),
            buffer.data(), buffer.size(), terminal, implement,
            isobus::CANIdentifier::CANPriority::Priority3);
    }

    void process_preferred_assignment(const isobus::CANMessage &message,
                                      const std::shared_ptr<isobus::ControlFunction> &implement)
    {
        std::uint8_t error = 0;
        std::uint16_t faulty_object = NULL_OBJECT_ID;
        std::size_t offset = 2U;
        std::vector<std::pair<aux_n::FunctionDescriptor, aux_n::InputDescriptor>> accepted;
        std::vector<std::uint16_t> used_functions;
        const std::uint8_t device_count = message.get_uint8_at(1);

        for (std::uint8_t device_index = 0; device_index < device_count; ++device_index)
        {
            if ((offset + 11U) > message.get_data_length())
            {
                error |= 0x10U;
                break;
            }
            std::uint64_t device_name = 0;
            for (std::uint8_t byte = 0; byte < 8U; ++byte)
            {
                device_name |= static_cast<std::uint64_t>(message.get_uint8_at(offset + byte)) << (8U * byte);
            }
            const std::uint16_t model = message.get_uint16_at(offset + 8U);
            const std::uint8_t assignment_count = message.get_uint8_at(offset + 10U);
            offset += 11U;

            std::lock_guard<std::mutex> lock(catalog_mutex);
            const auto device_control = control_functions.find(device_name);
            if ((device_control == control_functions.end()) || !device_control->second.lock())
            {
                error |= 0x01U;
            }
            for (std::uint8_t assignment_index = 0; assignment_index < assignment_count; ++assignment_index)
            {
                if ((offset + 4U) > message.get_data_length())
                {
                    error |= 0x10U;
                    break;
                }
                const std::uint16_t function_id = message.get_uint16_at(offset);
                const std::uint16_t input_id = message.get_uint16_at(offset + 2U);
                offset += 4U;
                const auto function = std::find_if(catalog.functions.begin(), catalog.functions.end(), [&](const aux_n::FunctionDescriptor &entry) {
                    return (entry.protocol == aux_n::Protocol::Type2) &&
                           (entry.implement_name == implement->get_NAME().get_full_name()) &&
                           (entry.object_id == function_id);
                });
                const auto input = std::find_if(catalog.inputs.begin(), catalog.inputs.end(), [&](const aux_n::InputDescriptor &entry) {
                    return (entry.protocol == aux_n::Protocol::Type2) && (entry.device_name == device_name) &&
                           (entry.model_identification_code == model) && (entry.object_id == input_id);
                });
                if (function == catalog.functions.end())
                {
                    error |= 0x02U;
                    faulty_object = function_id;
                }
                else if (input == catalog.inputs.end())
                {
                    error |= 0x04U;
                    faulty_object = function_id;
                }
                else if (std::find(used_functions.begin(), used_functions.end(), function_id) != used_functions.end())
                {
                    error |= 0x08U;
                    faulty_object = function_id;
                }
                else if ((function->function_type != input->function_type) || (function->critical && !input->critical))
                {
                    error |= 0x10U;
                    faulty_object = function_id;
                }
                else
                {
                    used_functions.push_back(function_id);
                    accepted.emplace_back(*function, *input);
                }
            }
        }

        const auto terminal = terminal_control_function.lock();
        if (terminal && implement)
        {
            const std::array<std::uint8_t, 8> response = { PREFERRED_ASSIGNMENT,
                                                           error,
                                                           static_cast<std::uint8_t>(faulty_object),
                                                           static_cast<std::uint8_t>(faulty_object >> 8U),
                                                           0xFF, 0xFF, 0xFF, 0xFF };
            isobus::CANNetworkManager::CANNetwork.send_can_message(
                static_cast<std::uint32_t>(isobus::CANLibParameterGroupNumber::VirtualTerminalToECU),
                response.data(), response.size(), terminal, implement,
                isobus::CANIdentifier::CANPriority::Priority3);
        }
        if (error == 0U)
        {
            for (const auto &assignment : accepted)
            {
                std::shared_ptr<isobus::ControlFunction> input_control;
                {
                    std::lock_guard<std::mutex> lock(catalog_mutex);
                    input_control = control_functions[assignment.second.device_name].lock();
                }
                if (input_control)
                {
                    send_status_enable(input_control, assignment.second.object_id, true);
                    if (send_assignment(implement,
                                        assignment.second.device_name,
                                        assignment.first.function_type,
                                        assignment.second.object_id,
                                        assignment.first.object_id,
                                        true))
                    {
                        std::lock_guard<std::mutex> lock(catalog_mutex);
                        remember_pending(assignment.first, assignment.second, true);
                    }
                }
            }
        }
        ESP_LOGI(TAG, "Preferred AUX-N assignment: devices=%u accepted=%u error=0x%02x",
                 static_cast<unsigned>(device_count), static_cast<unsigned>(accepted.size()), static_cast<unsigned>(error));
    }

private:
    using FunctionIterator = std::vector<aux_n::FunctionDescriptor>::iterator;

    FunctionIterator selected_function_locked()
    {
        return std::find_if(catalog.functions.begin(), catalog.functions.end(), [&](const aux_n::FunctionDescriptor &function) {
            return (function.protocol == catalog.selected_protocol) &&
                   (function.implement_name == catalog.selected_implement_name) &&
                   (function.object_id == catalog.selected_function_object_id);
        });
    }

    static bool is_compatible(const aux_n::FunctionDescriptor &function, const aux_n::InputDescriptor &input)
    {
        return (function.protocol == input.protocol) &&
               (function.function_type == input.function_type) &&
               (!function.critical || input.critical);
    }

    void remember_pending(const aux_n::FunctionDescriptor &function,
                          const aux_n::InputDescriptor &input,
                          bool preferred)
    {
        aux_n::Assignment assignment = { function.protocol, function.implement_name, function.object_id,
                                          input.device_name, input.object_id, false, preferred, 0 };
        const auto existing = std::find_if(catalog.assignments.begin(), catalog.assignments.end(), [&](const aux_n::Assignment &entry) {
            return (entry.implement_name == assignment.implement_name) &&
                   (entry.function_object_id == assignment.function_object_id);
        });
        if (existing == catalog.assignments.end())
        {
            catalog.assignments.push_back(assignment);
        }
        else
        {
            *existing = assignment;
        }
    }

    void save_assignments_locked()
    {
        std::vector<StoredAssignment> stored;
        for (const auto &entry : catalog.assignments)
        {
            if (entry.confirmed)
            {
                stored.push_back({ entry.implement_name, entry.device_name, entry.function_object_id,
                                   entry.input_object_id, static_cast<std::uint8_t>(entry.protocol),
                                   static_cast<std::uint8_t>(entry.preferred) });
            }
        }
        nvs_handle_t handle = 0;
        if (nvs_open(AUX_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK)
        {
            if (stored.empty())
            {
                nvs_erase_key(handle, AUX_NVS_ASSIGNMENTS);
            }
            else
            {
                nvs_set_blob(handle, AUX_NVS_ASSIGNMENTS, stored.data(), stored.size() * sizeof(StoredAssignment));
            }
            nvs_commit(handle);
            nvs_close(handle);
        }
    }

    void load_assignments()
    {
        nvs_handle_t handle = 0;
        if (nvs_open(AUX_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        {
            return;
        }
        std::size_t size = 0;
        if ((nvs_get_blob(handle, AUX_NVS_ASSIGNMENTS, nullptr, &size) == ESP_OK) &&
            (size > 0) && ((size % sizeof(StoredAssignment)) == 0))
        {
            std::vector<StoredAssignment> stored(size / sizeof(StoredAssignment));
            if (nvs_get_blob(handle, AUX_NVS_ASSIGNMENTS, stored.data(), &size) == ESP_OK)
            {
                std::lock_guard<std::mutex> lock(catalog_mutex);
                for (const auto &entry : stored)
                {
                    catalog.assignments.push_back({ static_cast<aux_n::Protocol>(entry.protocol), entry.implement_name,
                                                    entry.function_object_id, entry.device_name, entry.input_object_id,
                                                    true, entry.preferred != 0, 0 });
                }
            }
        }
        nvs_close(handle);
    }

    static void on_message(const isobus::CANMessage &message, void *parent)
    {
        if (parent != nullptr)
        {
            static_cast<Manager *>(parent)->process_message(message);
        }
    }

    void process_message(const isobus::CANMessage &message)
    {
        if (message.get_data_length() < 4U || !message.has_valid_source_control_function())
        {
            return;
        }

        const std::uint8_t function = message.get_uint8_at(0);
        const auto source = message.get_source_control_function();
        const std::uint64_t source_name = source->get_NAME().get_full_name();

        if ((function == PREFERRED_ASSIGNMENT) && (message.get_data_length() >= 8U))
        {
            process_preferred_assignment(message, source);
        }
        else if (function == AUXILIARY_INPUT_MAINTENANCE)
        {
            if (message.get_uint8_at(3) == 0U)
            {
                return;
            }

            const std::uint16_t model_code = message.get_uint16_at(1);
            bool discovered = false;
            {
                std::lock_guard<std::mutex> lock(devices_mutex);
                const auto device = std::find_if(input_devices.begin(), input_devices.end(), [source_name, model_code](const aux_n::InputDevice &entry) {
                    return (entry.name == source_name) && (entry.model_identification_code == model_code);
                });
                if (device == input_devices.end())
                {
                    input_devices.push_back({ source_name, model_code, esp_timer_get_time() });
                    discovered = true;
                }
                else
                {
                    device->last_seen_us = esp_timer_get_time();
                }
            }
            if (discovered)
            {
                {
                    std::lock_guard<std::mutex> lock(catalog_mutex);
                    catalog_signature = 0;
                }
                ESP_LOGI(TAG, "Discovered AUX-N input device NAME=0x%016llx model=%u",
                         static_cast<unsigned long long>(source_name),
                         model_code);
                vt_view::on_aux_n_input_device(source_name, model_code);
            }
        }
        else if ((function == AUXILIARY_ASSIGNMENT_TYPE_TWO) && (message.get_data_length() == 8U))
        {
            const std::uint16_t function_object_id = message.get_uint16_at(1);
            const std::uint8_t error_code = message.get_uint8_at(3);
            {
                std::lock_guard<std::mutex> lock(catalog_mutex);
                const std::pair<std::uint64_t, std::uint16_t> key = { source_name, function_object_id };
                if ((error_code == 0U) && (pending_removals.erase(key) > 0U))
                {
                    catalog.assignments.erase(std::remove_if(catalog.assignments.begin(), catalog.assignments.end(),
                                                              [&](const aux_n::Assignment &entry) {
                                                                  return (entry.implement_name == source_name) &&
                                                                         ((function_object_id == NULL_OBJECT_ID) ||
                                                                          (entry.function_object_id == function_object_id));
                                                              }),
                                              catalog.assignments.end());
                }
                else
                {
                    const auto assignment = std::find_if(catalog.assignments.begin(), catalog.assignments.end(),
                                                         [&](const aux_n::Assignment &entry) {
                                                             return (entry.implement_name == source_name) &&
                                                                    (entry.function_object_id == function_object_id);
                                                         });
                    if (assignment != catalog.assignments.end())
                    {
                        assignment->confirmed = error_code == 0U;
                        assignment->error_code = error_code;
                    }
                }
                save_assignments_locked();
            }
            vt_view::on_aux_n_assignment_result(source_name, function_object_id, error_code);
        }
        else if ((function == AUXILIARY_INPUT_STATUS_ENABLE) && (message.get_data_length() == 8U))
        {
            ESP_LOGI(TAG, "AUX-N status enable response NAME=0x%016llx input=%u error=0x%02x",
                     static_cast<unsigned long long>(source_name),
                     static_cast<unsigned>(message.get_uint16_at(1)),
                     static_cast<unsigned>(message.get_uint8_at(3)));
        }
        else if ((function == AUXILIARY_INPUT_STATUS_TYPE_TWO) && (message.get_data_length() == 8U))
        {
            const std::uint16_t input_object_id = message.get_uint16_at(1);
            const std::uint16_t value_1 = message.get_uint16_at(3);
            const std::uint16_t value_2 = message.get_uint16_at(5);
            const std::uint8_t state = message.get_uint8_at(7);
            bool assign_learned_input = false;
            {
                std::lock_guard<std::mutex> lock(catalog_mutex);
                auto status = std::find_if(catalog.input_statuses.begin(), catalog.input_statuses.end(),
                                           [&](const aux_n::InputStatus &entry) {
                                               return (entry.device_name == source_name) &&
                                                      (entry.input_object_id == input_object_id);
                                           });
                const bool moved = (status != catalog.input_statuses.end()) &&
                                   ((status->value_1 != value_1) || (status->value_2 != value_2));
                if (status == catalog.input_statuses.end())
                {
                    catalog.input_statuses.push_back({ source_name, input_object_id, value_1, value_2, state, esp_timer_get_time() });
                }
                else
                {
                    *status = { source_name, input_object_id, value_1, value_2, state, esp_timer_get_time() };
                }
                if (catalog.learning)
                {
                    const auto selected = selected_function_locked();
                    const auto input = std::find_if(catalog.inputs.begin(), catalog.inputs.end(), [&](const aux_n::InputDescriptor &entry) {
                        return (entry.protocol == aux_n::Protocol::Type2) &&
                               (entry.device_name == source_name) && (entry.object_id == input_object_id);
                    });
                    // Bit 1 is the ISO AUX-N "input activated in learn mode" flag.
                    // A value transition is accepted as a fallback for older devices.
                    assign_learned_input = (selected != catalog.functions.end()) && (input != catalog.inputs.end()) &&
                                           is_compatible(*selected, *input) && (((state & 0x02U) != 0U) || moved);
                }
            }
            if (assign_learned_input)
            {
                assign_selected(aux_n::Protocol::Type2, source_name, input_object_id);
            }
            ESP_LOGD(TAG, "AUX-N input NAME=0x%016llx id=%u value1=%u state=0x%02x",
                     static_cast<unsigned long long>(source_name),
                     static_cast<unsigned>(input_object_id),
                     static_cast<unsigned>(value_1),
                     static_cast<unsigned>(state));
        }
        else if ((function == AUXILIARY_ASSIGNMENT_TYPE_ONE) && (message.get_data_length() == 8U))
        {
            ESP_LOGI(TAG, "AUX-O assignment response NAME=0x%016llx function=%u error=0x%02x",
                     static_cast<unsigned long long>(source_name),
                     static_cast<unsigned>(message.get_uint16_at(3)),
                     static_cast<unsigned>(message.get_uint8_at(5)));
        }
    }

    std::weak_ptr<isobus::InternalControlFunction> terminal_control_function;
    mutable std::mutex devices_mutex;
    std::vector<aux_n::InputDevice> input_devices;
    std::mutex catalog_mutex;
    aux_n::Catalog catalog;
    std::map<std::uint64_t, std::weak_ptr<isobus::ControlFunction>> control_functions;
    std::set<std::pair<std::uint64_t, std::uint16_t>> pending_removals;
    std::set<std::pair<std::uint64_t, std::uint16_t>> restored_assignments;
    bool legacy_type1_enabled = false;
    std::int64_t learning_started_us = 0;
    std::uint64_t catalog_signature = 0;
    bool started = false;
};

Manager manager;
#endif
}

namespace aux_n
{
bool start(const std::shared_ptr<isobus::InternalControlFunction> &terminal_control_function)
{
#if ISOVT_ENABLE_AGISOSTACK
    return manager.start(terminal_control_function);
#else
    (void)terminal_control_function;
    return false;
#endif
}

std::vector<InputDevice> get_input_devices()
{
#if ISOVT_ENABLE_AGISOSTACK
    return manager.get_input_devices();
#else
    return {};
#endif
}

void refresh_catalog(
    const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &active_working_set,
    const std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> &working_sets)
{
#if ISOVT_ENABLE_AGISOSTACK
    manager.refresh_catalog(active_working_set, working_sets);
#else
    (void)active_working_set;
    (void)working_sets;
#endif
}

Catalog get_catalog()
{
#if ISOVT_ENABLE_AGISOSTACK
    return manager.get_catalog();
#else
    return {};
#endif
}

bool select_function(Protocol protocol, std::uint64_t implement_name, std::uint16_t function_object_id)
{
#if ISOVT_ENABLE_AGISOSTACK
    return manager.select_function(protocol, implement_name, function_object_id);
#else
    (void)protocol;
    (void)implement_name;
    (void)function_object_id;
    return false;
#endif
}

bool assign_selected(Protocol protocol, std::uint64_t device_name, std::uint16_t input_object_id)
{
#if ISOVT_ENABLE_AGISOSTACK
    return manager.assign_selected(protocol, device_name, input_object_id);
#else
    (void)protocol;
    (void)device_name;
    (void)input_object_id;
    return false;
#endif
}

bool begin_learning()
{
#if ISOVT_ENABLE_AGISOSTACK
    return manager.begin_learning();
#else
    return false;
#endif
}

void cancel_learning()
{
#if ISOVT_ENABLE_AGISOSTACK
    manager.cancel_learning();
#endif
}

bool unassign_selected()
{
#if ISOVT_ENABLE_AGISOSTACK
    return manager.unassign_selected();
#else
    return false;
#endif
}

void set_legacy_type1_enabled(bool enabled)
{
#if ISOVT_ENABLE_AGISOSTACK
    manager.set_legacy_type1_enabled(enabled);
#else
    (void)enabled;
#endif
}

bool get_legacy_type1_enabled()
{
#if ISOVT_ENABLE_AGISOSTACK
    return manager.get_catalog().legacy_type1_enabled;
#else
    return false;
#endif
}

bool assign(const std::shared_ptr<isobus::ControlFunction> &implement,
            std::uint64_t input_device_name,
            FunctionType function_type,
            std::uint16_t input_object_id,
            std::uint16_t function_object_id,
            bool store_as_preferred)
{
#if ISOVT_ENABLE_AGISOSTACK
    return manager.send_assignment(implement,
                                   input_device_name,
                                   static_cast<std::uint8_t>(function_type),
                                   input_object_id,
                                   function_object_id,
                                   store_as_preferred);
#else
    (void)implement;
    (void)input_device_name;
    (void)function_type;
    (void)input_object_id;
    (void)function_object_id;
    (void)store_as_preferred;
    return false;
#endif
}

bool unassign_function(const std::shared_ptr<isobus::ControlFunction> &implement,
                       std::uint16_t function_object_id,
                       bool store_as_preferred)
{
#if ISOVT_ENABLE_AGISOSTACK
    return manager.send_assignment(implement,
                                   NULL_DEVICE_NAME,
                                   REMOVE_ASSIGNMENT,
                                   NULL_OBJECT_ID,
                                   function_object_id,
                                   store_as_preferred);
#else
    (void)implement;
    (void)function_object_id;
    (void)store_as_preferred;
    return false;
#endif
}

bool unassign_all(const std::shared_ptr<isobus::ControlFunction> &implement, bool store_as_preferred)
{
#if ISOVT_ENABLE_AGISOSTACK
    return manager.unassign_all_tracked(implement, store_as_preferred);
#else
    (void)implement;
    (void)store_as_preferred;
    return false;
#endif
}
}
