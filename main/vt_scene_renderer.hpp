#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace isobus
{
class VirtualTerminalServerManagedWorkingSet;
}

namespace vt_scene_renderer
{
enum class HitKind : std::uint8_t
{
    None,
    Button,
    SoftKey,
    Input,
    InputBoolean,
    InputList,
    Editor,
    WorkingSet,
    Terminal,
    AuxFunction,
    AuxInput,
};

enum class InputEditorKind : std::uint8_t
{
    None,
    Number,
    Text,
    List,
};

enum class EditorAction : std::uint8_t
{
    None,
    Character,
    Backspace,
    Clear,
    Previous,
    Next,
    Select,
    Cancel,
    Confirm,
};

enum class TerminalAction : std::uint8_t
{
    None,
    ToggleMenu,
    ShowMachines,
    ShowTerminal,
    ShowIsobus,
    ShowAuxN,
    ShowGnss,
    ShowAudio,
    ShowInformation,
    CycleLanguage,
    CycleDateFormat,
    ToggleTimeFormat,
    ToggleDecimalSymbol,
    ToggleUnits,
    CycleTerminalNumber,
    CycleDataMaskSize,
    DecreaseControlVolume,
    IncreaseControlVolume,
    DecreaseAlarmVolume,
    IncreaseAlarmVolume,
    ToggleAudioMute,
    TestControlAudio,
    TestAlarmAudio,
    RefreshNetwork,
    EraseActivePool,
    EraseAllPools,
    ClearAuxAssignments,
    UnassignSelectedAux,
    ToggleAuxLearn,
    AuxFunctionPreviousPage,
    AuxFunctionNextPage,
    AuxInputPreviousPage,
    AuxInputNextPage,
    ToggleAuxOLegacy,
};

enum class TerminalLanguage : std::uint8_t
{
    Polish,
    English,
    German,
    French,
    Hungarian,
    Dutch,
    Russian,
    Lithuanian,
    Count,
};

enum class TerminalDateFormat : std::uint8_t
{
    DayMonthYear,
    MonthDayYear,
    YearMonthDay,
};

struct TerminalSettings
{
    TerminalLanguage language = TerminalLanguage::Polish;
    TerminalDateFormat date_format = TerminalDateFormat::DayMonthYear;
    bool twenty_four_hour = true;
    bool decimal_comma = true;
    bool metric_units = true;
    std::uint8_t terminal_number = 1;
    std::uint16_t data_mask_size = 480;
    std::uint8_t control_volume = 70;
    std::uint8_t alarm_volume = 90;
    bool audio_muted = false;
    bool aux_o_legacy = false;
};

struct HitTarget
{
    HitKind kind = HitKind::None;
    std::uint16_t object_id = 0xFFFF;
    std::uint16_t parent_id = 0xFFFF;
    std::uint8_t key_code = 0;
    std::uint64_t working_set_name = 0;
    TerminalAction terminal_action = TerminalAction::None;
    EditorAction editor_action = EditorAction::None;
    char editor_character = '\0';
    std::uint16_t editor_index = 0;
    std::uint8_t aux_protocol = 0;
};

enum class EditorTextEncoding : std::uint8_t
{
    ISO8859_1,
    ISO8859_15,
    ISO8859_2,
    ISO8859_4,
    ISO8859_5,
    ISO8859_7,
};

struct InputEditorResult
{
    bool handled = false;
    bool committed = false;
    bool cancelled = false;
    InputEditorKind kind = InputEditorKind::None;
    std::uint16_t object_id = 0xFFFF;
    std::string value;
    std::uint16_t selected_index = 0;
    EditorTextEncoding text_encoding = EditorTextEncoding::ISO8859_1;
};

struct EditorListItem
{
    std::string label;
    std::uint16_t object_id = 0xFFFF;
    EditorTextEncoding encoding = EditorTextEncoding::ISO8859_1;
};

bool render(const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set,
            const std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> &working_sets,
            const std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> &auxiliary_working_sets = {});
HitTarget hit_test(std::uint16_t x, std::uint16_t y);
bool is_menu_open();
void apply_terminal_action(TerminalAction action);
void close_menu();
void set_terminal_settings(const TerminalSettings &settings);
TerminalSettings get_terminal_settings();
void set_metric_units(bool metric);
bool get_metric_units();
void set_notice(const char *text);
void open_input_editor(InputEditorKind kind,
                       std::uint16_t object_id,
                       const std::string &initial_value,
                       std::size_t maximum_length = 32,
                       const std::vector<EditorListItem> &list_items = {},
                       std::uint16_t selected_index = 0,
                       EditorTextEncoding text_encoding = EditorTextEncoding::ISO8859_1);
InputEditorResult apply_editor_action(EditorAction action, char character = '\0', std::uint16_t index = 0);
bool is_input_editor_open();
void close_input_editor();
void clear();
}
