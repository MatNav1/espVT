#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace isobus
{
class ControlFunction;
class InternalControlFunction;
class VirtualTerminalServerManagedWorkingSet;
}

namespace aux_n
{
enum class Protocol : std::uint8_t
{
    Type2,
    LegacyType1,
};

enum class FunctionType : std::uint8_t
{
    BooleanLatching = 0,
    AnalogueLatching = 1,
    BooleanMomentary = 2,
    AnalogueMomentaryTwoWay = 3,
    AnalogueMomentaryOneWay = 4,
    DualBooleanLatching = 5,
    DualBooleanMomentary = 6,
    DualBooleanLatchingUpOnly = 7,
    DualBooleanLatchingDownOnly = 8,
    AnalogueMomentaryBooleanLatching = 9,
    AnalogueLatchingBooleanLatching = 10,
    QuadratureBooleanMomentary = 11,
    QuadratureAnalogueLatching = 12,
    QuadratureAnalogueMomentary = 13,
    BidirectionalEncoder = 14,
};

struct InputDevice
{
    std::uint64_t name = 0;
    std::uint16_t model_identification_code = 0;
    std::int64_t last_seen_us = 0;
};

struct FunctionDescriptor
{
    Protocol protocol = Protocol::Type2;
    std::uint64_t implement_name = 0;
    std::uint16_t object_id = 0xFFFF;
    std::uint8_t function_type = 0;
    bool critical = false;
    bool restricted = false;
    bool single_assignment = false;
};

struct InputDescriptor
{
    Protocol protocol = Protocol::Type2;
    std::uint64_t device_name = 0;
    std::uint16_t model_identification_code = 0;
    std::uint16_t object_id = 0xFFFF;
    std::uint8_t input_number = 0xFF;
    std::uint8_t function_type = 0;
    bool critical = false;
    bool single_assignment = false;
    bool online = false;
};

struct Assignment
{
    Protocol protocol = Protocol::Type2;
    std::uint64_t implement_name = 0;
    std::uint16_t function_object_id = 0xFFFF;
    std::uint64_t device_name = 0;
    std::uint16_t input_object_id = 0xFFFF;
    bool confirmed = false;
    bool preferred = false;
    std::uint8_t error_code = 0;
};

struct InputStatus
{
    std::uint64_t device_name = 0;
    std::uint16_t input_object_id = 0xFFFF;
    std::uint16_t value_1 = 0;
    std::uint16_t value_2 = 0;
    std::uint8_t state = 0;
    std::int64_t last_seen_us = 0;
};

struct Catalog
{
    std::vector<FunctionDescriptor> functions;
    std::vector<InputDescriptor> inputs;
    std::vector<Assignment> assignments;
    std::vector<InputStatus> input_statuses;
    std::uint64_t selected_implement_name = 0;
    std::uint16_t selected_function_object_id = 0xFFFF;
    Protocol selected_protocol = Protocol::Type2;
    bool legacy_type1_enabled = false;
    bool learning = false;
};

bool start(const std::shared_ptr<isobus::InternalControlFunction> &terminal_control_function);
std::vector<InputDevice> get_input_devices();
void refresh_catalog(
    const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &active_working_set,
    const std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> &working_sets);
Catalog get_catalog();
bool select_function(Protocol protocol, std::uint64_t implement_name, std::uint16_t function_object_id);
bool assign_selected(Protocol protocol, std::uint64_t device_name, std::uint16_t input_object_id);
bool begin_learning();
void cancel_learning();
bool unassign_selected();
void set_legacy_type1_enabled(bool enabled);
bool get_legacy_type1_enabled();

bool assign(const std::shared_ptr<isobus::ControlFunction> &implement,
            std::uint64_t input_device_name,
            FunctionType function_type,
            std::uint16_t input_object_id,
            std::uint16_t function_object_id,
            bool store_as_preferred);

bool unassign_function(const std::shared_ptr<isobus::ControlFunction> &implement,
                       std::uint16_t function_object_id,
                       bool store_as_preferred);
bool unassign_all(const std::shared_ptr<isobus::ControlFunction> &implement, bool store_as_preferred);
}
