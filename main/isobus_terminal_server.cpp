#include "isobus_terminal_server.hpp"

#include "aux_n_manager.hpp"
#include "board_pins.hpp"
#include "can_monitor.hpp"
#include "gnss_um982.hpp"
#include "lvgl_terminal_view.hpp"
#include "object_pool_store.hpp"
#include "vt_scene_renderer.hpp"
#include "vt_audio.hpp"

#include "esp_log.h"
#include "nvs.h"

#if ISOVT_ENABLE_AGISOSTACK
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "isobus/isobus/can_NAME.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/isobus/isobus_virtual_terminal_objects.hpp"
#include "isobus/isobus/isobus_virtual_terminal_server.hpp"
#include "isobus/isobus/isobus_speed_distance_messages.hpp"
#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/utility/system_timing.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#endif

namespace
{
constexpr char TAG[] = "isovt";

#if ISOVT_ENABLE_AGISOSTACK
constexpr std::uint8_t VT_PREFERRED_ADDRESS = 0x26;
constexpr std::uint32_t OBJECT_POOL_PROGRESS_UPDATE_MS = 200;

std::uint16_t configured_data_mask_size()
{
    return vt_scene_renderer::get_terminal_settings().data_mask_size == 600 ? 600 : 480;
}

std::uint8_t configured_soft_key_size()
{
    return static_cast<std::uint8_t>(configured_data_mask_size() / 8U);
}

std::uint32_t expected_object_pool_bytes = 0;
std::uint32_t received_object_pool_bytes = 0;
std::uint32_t last_object_pool_progress_timestamp_ms = 0;
std::uint8_t last_object_pool_percent = 0;

void begin_object_pool_progress(std::uint32_t expected_bytes);
void finish_object_pool_progress();
bool render_working_set(const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set);

std::uint64_t client_name(const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set)
{
    const auto control_function = working_set ? working_set->get_control_function() : nullptr;
    return control_function ? control_function->get_NAME().get_full_name() : 0;
}

class EspCanLogger final : public isobus::CANStackLogger
{
public:
    void sink_CAN_stack_log(LoggingLevel level, const std::string &text) override
    {
        switch (level)
        {
            case LoggingLevel::Debug:
                ESP_LOGD(TAG, "%s", text.c_str());
                break;
            case LoggingLevel::Info:
                ESP_LOGI(TAG, "%s", text.c_str());
                break;
            case LoggingLevel::Warning:
                ESP_LOGW(TAG, "%s", text.c_str());
                break;
            case LoggingLevel::Error:
            case LoggingLevel::Critical:
                ESP_LOGE(TAG, "%s", text.c_str());
                break;
        }
    }
};

class EspVirtualTerminalServer final : public isobus::VirtualTerminalServer
{
public:
    explicit EspVirtualTerminalServer(std::shared_ptr<isobus::InternalControlFunction> control_function) :
      VirtualTerminalServer(std::move(control_function))
    {
    }

    void update_server()
    {
        update();
    }

    std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> get_renderable_working_sets() const
    {
		const std::lock_guard<std::recursive_mutex> working_set_list_lock(managedWorkingSetListMutex);
        std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> result;
        result.reserve(managedWorkingSetList.size());
        for (const auto &working_set : managedWorkingSetList)
        {
            if (is_selectable_working_set(working_set))
            {
                result.push_back(working_set);
            }
        }
        return result;
    }

    std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> get_all_working_sets() const
    {
        const std::lock_guard<std::recursive_mutex> working_set_list_lock(managedWorkingSetListMutex);
        std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> result;
        for (const auto &working_set : managedWorkingSetList)
        {
            // The parser mutates the object map on another thread. AUX catalog
            // scanning and rendering must wait until it has finished.
            using State = isobus::VirtualTerminalServerManagedWorkingSet::ObjectPoolProcessingThreadState;
            const auto state = working_set->get_object_pool_processing_state();
            if ((state == State::Success) || (state == State::Joined))
            {
                result.push_back(working_set);
            }
        }
        return result;
    }

    bool ensure_active_working_set(const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set)
    {
        if (ui_active_working_set)
        {
            sync_active_masks(ui_active_working_set);
            return true;
        }
        return is_selectable_working_set(working_set) && activate_working_set(working_set);
    }

    std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> get_ui_active_working_set() const
    {
        return ui_active_working_set;
    }

    bool deactivate_working_set(std::uint64_t name)
    {
        if (!ui_active_working_set || (client_name(ui_active_working_set) != name))
        {
            return false;
        }

        ESP_LOGI(TAG, "Hiding inactive working set 0x%016llx without deleting its cached pool",
                 static_cast<unsigned long long>(name));
        ui_active_working_set.reset();
        activeWorkingSet.reset();
        activeWorkingSetMasterAddress = isobus::NULL_CAN_ADDRESS;
        activeWorkingSetDataMaskObjectID = isobus::NULL_OBJECT_ID;
        activeWorkingSetSoftkeyMaskObjectID = isobus::NULL_OBJECT_ID;
        vt_audio::set_alarm_signal(static_cast<std::uint8_t>(isobus::AlarmMask::AcousticSignal::None));
        return true;
    }

    bool activate_working_set(std::uint64_t name)
    {
		const std::lock_guard<std::recursive_mutex> working_set_list_lock(managedWorkingSetListMutex);
        for (const auto &working_set : managedWorkingSetList)
        {
            if ((client_name(working_set) == name) && is_selectable_working_set(working_set))
            {
                return activate_working_set(working_set);
            }
        }
        return false;
    }

    bool activate_working_set(const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set)
    {
        if (!is_selectable_working_set(working_set))
        {
            return false;
        }
        const auto previous_working_set = ui_active_working_set;
        if (previous_working_set && (previous_working_set != working_set))
        {
            notify_working_set_visibility(previous_working_set, false);
        }
        ui_active_working_set = working_set;
        activeWorkingSet = working_set;
        activeWorkingSetMasterAddress = working_set->get_control_function()->get_address();
        sync_active_masks(working_set);
        notify_working_set_visibility(working_set, true);
        ESP_LOGI(TAG, "Active working set changed to 0x%016llx (SA %u)",
                 static_cast<unsigned long long>(client_name(working_set)),
                 static_cast<unsigned>(activeWorkingSetMasterAddress));
        return true;
    }

    bool is_selectable_working_set(
        const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set) const
    {
        if (!working_set || !working_set->get_control_function())
        {
            return false;
        }
        using State = isobus::VirtualTerminalServerManagedWorkingSet::ObjectPoolProcessingThreadState;
        const auto state = working_set->get_object_pool_processing_state();
        if ((state != State::Success) && (state != State::Joined))
        {
            return false;
        }
        const auto object = working_set->get_working_set_object();
        return object &&
               (object->get_object_type() == isobus::VirtualTerminalObjectType::WorkingSet) &&
               std::static_pointer_cast<isobus::WorkingSet>(object)->get_selectable();
    }

    void sync_active_masks(const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set)
    {
        if ((working_set != ui_active_working_set) || !working_set || !working_set->get_working_set_object())
        {
            return;
        }

        const auto working_set_object = std::static_pointer_cast<isobus::WorkingSet>(working_set->get_working_set_object());
        activeWorkingSetDataMaskObjectID = working_set_object->get_active_mask();
        activeWorkingSetSoftkeyMaskObjectID = isobus::NULL_OBJECT_ID;
        const auto data_mask = working_set->get_object_by_id(activeWorkingSetDataMaskObjectID);
        if (data_mask && (data_mask->get_object_type() == isobus::VirtualTerminalObjectType::DataMask))
        {
            activeWorkingSetSoftkeyMaskObjectID = std::static_pointer_cast<isobus::DataMask>(data_mask)->get_soft_key_mask();
            vt_audio::set_alarm_signal(static_cast<std::uint8_t>(isobus::AlarmMask::AcousticSignal::None));
        }
        else if (data_mask && (data_mask->get_object_type() == isobus::VirtualTerminalObjectType::AlarmMask))
        {
            const auto alarm_mask = std::static_pointer_cast<isobus::AlarmMask>(data_mask);
            activeWorkingSetSoftkeyMaskObjectID = alarm_mask->get_soft_key_mask();
            vt_audio::set_alarm_signal(static_cast<std::uint8_t>(alarm_mask->get_signal_priority()));
        }
        // While an object pool is being transferred or parsed, its active mask
        // can be temporarily unresolved. Keep the previous alarm state then;
        // toggling None/alarm here would replay the alarm on every refresh.
    }

    void announce_active_layout_visibility(
        const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set)
    {
        notify_working_set_visibility(working_set, true, false);
    }

private:
    void identify_vt() override
    {
        ESP_LOGI(TAG, "Identify VT requested: terminal number %u",
                 static_cast<unsigned>(vt_scene_renderer::get_terminal_settings().terminal_number));
        vt_scene_renderer::set_notice("IDENTYFIKACJA TERMINALA VT");
        render_working_set(ui_active_working_set);
    }

    void screen_capture(std::uint8_t item,
                        std::uint8_t path,
                        std::shared_ptr<isobus::ControlFunction> requestor) override
    {
        // The microSD connector is used for GNSS/audio GPIO in this hardware build.
        const auto error = static_cast<std::uint8_t>(ScreenCaptureResponseErrorBit::RemovableMediaUnavailable);
        send_capture_screen_response(item, path, error, 0, std::move(requestor));
    }

    void control_audio_signal(std::uint8_t activations,
                              std::uint16_t frequency_hz,
                              std::uint16_t duration_ms,
                              std::uint16_t off_time_ms,
                              std::shared_ptr<isobus::ControlFunction>) override
    {
        vt_audio::play_control_signal(activations, frequency_hz, duration_ms, off_time_ms);
    }

    void set_audio_volume(std::uint8_t volume_percent,
                          std::shared_ptr<isobus::ControlFunction>) override
    {
        vt_audio::set_requested_control_volume(volume_percent);
    }

    void notify_working_set_visibility(
        const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set,
        bool visible,
        bool process_events = true)
    {
        if (!working_set || !working_set->get_control_function() || !working_set->get_working_set_object())
        {
            return;
        }

        const auto working_set_object = std::static_pointer_cast<isobus::WorkingSet>(working_set->get_working_set_object());
        const auto data_mask = working_set->get_object_by_id(working_set_object->get_active_mask());
        std::uint16_t soft_key_mask_id = isobus::NULL_OBJECT_ID;
        if (data_mask && (data_mask->get_object_type() == isobus::VirtualTerminalObjectType::DataMask))
        {
            soft_key_mask_id = std::static_pointer_cast<isobus::DataMask>(data_mask)->get_soft_key_mask();
        }
        else if (data_mask && (data_mask->get_object_type() == isobus::VirtualTerminalObjectType::AlarmMask))
        {
            soft_key_mask_id = std::static_pointer_cast<isobus::AlarmMask>(data_mask)->get_soft_key_mask();
        }
        const auto soft_key_mask = working_set->get_object_by_id(soft_key_mask_id);

        std::array<std::uint8_t, isobus::CAN_DATA_LENGTH> message = {
            static_cast<std::uint8_t>(Function::VTOnUserLayoutHideShowMessage),
            static_cast<std::uint8_t>(working_set_object->get_active_mask() & 0xFFU),
            static_cast<std::uint8_t>(working_set_object->get_active_mask() >> 8U),
            static_cast<std::uint8_t>(visible ? 1 : 0),
            static_cast<std::uint8_t>(soft_key_mask_id & 0xFFU),
            static_cast<std::uint8_t>(soft_key_mask_id >> 8U),
            static_cast<std::uint8_t>(visible ? 1 : 0),
            0x0F,
        };
        isobus::CANNetworkManager::CANNetwork.send_can_message(
            static_cast<std::uint32_t>(isobus::CANLibParameterGroupNumber::VirtualTerminalToECU),
            message.data(),
            message.size(),
            get_internal_control_function(),
            working_set->get_control_function(),
            get_priority());

        if (!process_events)
        {
            return;
        }

        const auto event = visible ? isobus::EventID::OnShow : isobus::EventID::OnHide;
        if (data_mask)
        {
            process_macro(data_mask, event, data_mask->get_object_type(), working_set);
        }
        if (soft_key_mask)
        {
            process_macro(soft_key_mask, event, isobus::VirtualTerminalObjectType::SoftKeyMask, working_set);
        }
    }

    std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ui_active_working_set;

public:
    bool get_is_enough_memory(std::uint32_t requested_memory) const override
    {
        const bool enough_memory = requested_memory <= object_pool_store::available_bytes();
        if (enough_memory && (requested_memory > 0))
        {
            begin_object_pool_progress(requested_memory);
        }
        return enough_memory;
    }

    VTVersion get_version() const override
    {
        return vt_scene_renderer::get_terminal_settings().aux_o_legacy ? VTVersion::Version2OrOlder : VTVersion::Version6;
    }

    std::uint8_t get_number_of_navigation_soft_keys() const override
    {
        return 0;
    }

    std::uint8_t get_soft_key_descriptor_x_pixel_width() const override
    {
        return configured_soft_key_size();
    }

    std::uint8_t get_soft_key_descriptor_y_pixel_height() const override
    {
        return configured_soft_key_size();
    }

    std::uint8_t get_number_of_possible_virtual_soft_keys_in_soft_key_mask() const override
    {
        return 12;
    }

    std::uint8_t get_number_of_physical_soft_keys() const override
    {
        return 12;
    }

    std::uint16_t get_data_mask_area_size_x_pixels() const override
    {
        return configured_data_mask_size();
    }

    std::uint16_t get_data_mask_area_size_y_pixels() const override
    {
        return configured_data_mask_size();
    }

    void suspend_working_set(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> working_set) override
    {
        ESP_LOGW(TAG, "Suspending faulty working set 0x%016llx",
                 static_cast<unsigned long long>(client_name(working_set)));
        vt_view::set_status("Working set suspended after a VT protocol error");
    }

    SupportedWideCharsErrorCode get_supported_wide_chars(std::uint8_t code_plane,
                                                           std::uint16_t,
                                                           std::uint16_t,
                                                           std::uint8_t &number_of_ranges,
                                                           std::vector<std::uint8_t> &wide_char_ranges) override
    {
        number_of_ranges = 0;
        wide_char_ranges.clear();
        return code_plane == 0 ? static_cast<SupportedWideCharsErrorCode>(0) : SupportedWideCharsErrorCode::ErrorInCodePlane;
    }

    std::vector<std::array<std::uint8_t, 7>> get_versions(isobus::NAME client) override
    {
        return object_pool_store::list_versions(client.get_full_name());
    }

    std::vector<std::uint8_t> get_supported_objects() const override
    {
        using Type = isobus::VirtualTerminalObjectType;
        return {
            static_cast<std::uint8_t>(Type::WorkingSet),
            static_cast<std::uint8_t>(Type::DataMask),
            static_cast<std::uint8_t>(Type::AlarmMask),
            static_cast<std::uint8_t>(Type::Container),
            static_cast<std::uint8_t>(Type::SoftKeyMask),
            static_cast<std::uint8_t>(Type::Key),
            static_cast<std::uint8_t>(Type::Button),
            static_cast<std::uint8_t>(Type::InputBoolean),
            static_cast<std::uint8_t>(Type::InputString),
            static_cast<std::uint8_t>(Type::InputNumber),
            static_cast<std::uint8_t>(Type::InputList),
            static_cast<std::uint8_t>(Type::OutputString),
            static_cast<std::uint8_t>(Type::OutputNumber),
            static_cast<std::uint8_t>(Type::OutputLine),
            static_cast<std::uint8_t>(Type::OutputRectangle),
            static_cast<std::uint8_t>(Type::OutputEllipse),
            static_cast<std::uint8_t>(Type::OutputPolygon),
            static_cast<std::uint8_t>(Type::OutputMeter),
            static_cast<std::uint8_t>(Type::OutputLinearBarGraph),
            static_cast<std::uint8_t>(Type::OutputArchedBarGraph),
            static_cast<std::uint8_t>(Type::PictureGraphic),
            static_cast<std::uint8_t>(Type::NumberVariable),
            static_cast<std::uint8_t>(Type::StringVariable),
            static_cast<std::uint8_t>(Type::FontAttributes),
            static_cast<std::uint8_t>(Type::LineAttributes),
            static_cast<std::uint8_t>(Type::FillAttributes),
            static_cast<std::uint8_t>(Type::InputAttributes),
            static_cast<std::uint8_t>(Type::ObjectPointer),
            static_cast<std::uint8_t>(Type::Macro),
            static_cast<std::uint8_t>(Type::AuxiliaryFunctionType1),
            static_cast<std::uint8_t>(Type::AuxiliaryInputType1),
            static_cast<std::uint8_t>(Type::AuxiliaryFunctionType2),
            static_cast<std::uint8_t>(Type::AuxiliaryInputType2),
            static_cast<std::uint8_t>(Type::AuxiliaryControlDesignatorType2),
            static_cast<std::uint8_t>(Type::WindowMask),
            static_cast<std::uint8_t>(Type::KeyGroup),
            static_cast<std::uint8_t>(Type::GraphicsContext),
            static_cast<std::uint8_t>(Type::OutputList),
            static_cast<std::uint8_t>(Type::ExtendedInputAttributes),
            static_cast<std::uint8_t>(Type::ColourMap),
            static_cast<std::uint8_t>(Type::ObjectLabelRefrenceList),
            static_cast<std::uint8_t>(Type::ExternalObjectDefinition),
            static_cast<std::uint8_t>(Type::ExternalReferenceNAME),
            static_cast<std::uint8_t>(Type::ExternalObjectPointer),
            static_cast<std::uint8_t>(Type::Animation),
            static_cast<std::uint8_t>(Type::GraphicData),
            static_cast<std::uint8_t>(Type::ScaledGraphic),
        };
    }

    std::vector<std::uint8_t> load_version(const std::vector<std::uint8_t> &version_label, isobus::NAME client) override
    {
        return object_pool_store::load(client.get_full_name(), version_label);
    }

    bool save_version(const std::vector<std::uint8_t> &object_pool,
                      const std::vector<std::uint8_t> &version_label,
                      isobus::NAME client) override
    {
        return object_pool_store::save(client.get_full_name(), version_label, object_pool);
    }

    bool delete_version(const std::vector<std::uint8_t> &version_label, isobus::NAME client) override
    {
        return object_pool_store::erase(client.get_full_name(), version_label);
    }

    bool delete_all_versions(isobus::NAME client) override
    {
        return object_pool_store::erase_all(client.get_full_name());
    }

    bool delete_object_pool(isobus::NAME client) override
    {
        ESP_LOGI(TAG, "Deactivating volatile object pool for 0x%016llx",
                 static_cast<unsigned long long>(client.get_full_name()));
        expected_object_pool_bytes = 0;
        vt_view::set_object_pool_download_progress(false, 0);
        vt_scene_renderer::clear();
        return true;
    }

    void transferred_object_pool_parse_start(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set) const override
    {
        finish_object_pool_progress();
        vt_view::set_status("Parsing ISOBUS object pool");
        vt_view::on_object_pool_loaded(client_name(working_set), 0);
    }

    void transferred_object_pool_component_received(
        std::uint32_t bytes,
        std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set) const override
    {
        (void)working_set;
        if (expected_object_pool_bytes == 0)
        {
            return;
        }

        received_object_pool_bytes = std::min(
            expected_object_pool_bytes,
            received_object_pool_bytes + bytes);
        const auto percent = static_cast<std::uint8_t>(std::min<std::uint64_t>(
            99,
            (static_cast<std::uint64_t>(received_object_pool_bytes) * 100U) /
                expected_object_pool_bytes));
        if (percent != last_object_pool_percent)
        {
            last_object_pool_percent = percent;
            vt_view::set_object_pool_download_progress(true, percent);
        }
    }
};

EspCanLogger stack_logger;
std::shared_ptr<isobus::InternalControlFunction> terminal_control_function;
std::shared_ptr<EspVirtualTerminalServer> terminal_server;
std::shared_ptr<isobus::InternalControlFunction> speed_control_function;
std::unique_ptr<isobus::SpeedMessagesInterface> speed_messages;
bool vt_update_callback_registered = false;
std::mutex touch_mutex;
std::weak_ptr<isobus::VirtualTerminalServerManagedWorkingSet> rendered_working_set;
vt_scene_renderer::HitTarget pressed_target;
std::uint32_t last_key_held_timestamp_ms = 0;
std::mutex render_request_mutex;
std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> pending_render_working_set;
std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> pending_render_working_sets;
std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> pending_auxiliary_working_sets;
std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> post_transition_render_working_set;
std::uint32_t post_transition_render_timestamp_ms = 0;
std::uint8_t post_transition_renders_remaining = 0;
TaskHandle_t working_set_render_task_handle = nullptr;
std::atomic<std::uint64_t> pending_working_set_selection{ 0 };
std::mutex inactive_machine_mutex;
std::vector<std::pair<std::uint64_t, std::uint8_t>> pending_inactive_machines;
std::uint32_t last_terminal_menu_refresh_ms = 0;
constexpr char TERMINAL_SETTINGS_NAMESPACE[] = "vt_settings";
constexpr std::uint32_t FIRST_POST_TRANSITION_RENDER_DELAY_MS = 100;
constexpr std::uint32_t SECOND_POST_TRANSITION_RENDER_DELAY_MS = 350;
constexpr std::uint32_t THIRD_POST_TRANSITION_RENDER_DELAY_MS = 900;
constexpr std::uint32_t RENDER_COALESCE_DELAY_MS = 20;
constexpr std::uint32_t RENDER_RETRY_DELAY_MS = 25;
constexpr std::uint32_t KEY_STILL_HELD_INTERVAL_MS = 200;
constexpr std::uint8_t SPEED_PROVIDER_PREFERRED_ADDRESS = 0x27;
constexpr std::int64_t GNSS_VELOCITY_TIMEOUT_MS = 2500;
constexpr std::uint32_t SPEED_STATUS_LOG_INTERVAL_MS = 5000;
constexpr std::uint64_t SPEED_DISTANCE_ROLLOVER_MM = 4211081216ULL;

std::uint64_t speed_distance_mm = 0;
std::uint32_t speed_last_update_ms = 0;
std::uint32_t speed_last_log_ms = 0;
bool speed_was_valid = false;

bool render_working_set(const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set);

void schedule_post_transition_renders(
    const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set)
{
    if (!working_set)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(render_request_mutex);
    post_transition_render_working_set = working_set;
    post_transition_render_timestamp_ms = isobus::SystemTiming::get_timestamp_ms();
    post_transition_renders_remaining = 3;
}

void service_post_transition_renders()
{
    std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> working_set;
    {
        std::lock_guard<std::mutex> lock(render_request_mutex);
        if (!post_transition_render_working_set || (post_transition_renders_remaining == 0))
        {
            return;
        }
        const std::uint32_t delay = (post_transition_renders_remaining == 3) ?
                                      FIRST_POST_TRANSITION_RENDER_DELAY_MS :
                                    (post_transition_renders_remaining == 2) ?
                                      SECOND_POST_TRANSITION_RENDER_DELAY_MS :
                                      THIRD_POST_TRANSITION_RENDER_DELAY_MS;
        if (!isobus::SystemTiming::time_expired_ms(post_transition_render_timestamp_ms, delay))
        {
            return;
        }
        working_set = post_transition_render_working_set;
        --post_transition_renders_remaining;
        post_transition_render_timestamp_ms = isobus::SystemTiming::get_timestamp_ms();
        if (post_transition_renders_remaining == 0)
        {
            post_transition_render_working_set.reset();
        }
    }
    render_working_set(working_set);
}

void load_terminal_settings()
{
    vt_scene_renderer::TerminalSettings settings;
    nvs_handle_t handle = 0;
    if (nvs_open(TERMINAL_SETTINGS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK)
    {
        std::uint8_t value = 0;
        if ((nvs_get_u8(handle, "language", &value) == ESP_OK) &&
            (value < static_cast<std::uint8_t>(vt_scene_renderer::TerminalLanguage::Count)))
        {
            settings.language = static_cast<vt_scene_renderer::TerminalLanguage>(value);
        }
        if ((nvs_get_u8(handle, "date", &value) == ESP_OK) && (value <= 2U))
        {
            settings.date_format = static_cast<vt_scene_renderer::TerminalDateFormat>(value);
        }
        if (nvs_get_u8(handle, "time24", &value) == ESP_OK)
        {
            settings.twenty_four_hour = value != 0U;
        }
        if (nvs_get_u8(handle, "decimal", &value) == ESP_OK)
        {
            settings.decimal_comma = value != 0U;
        }
        if (nvs_get_u8(handle, "metric", &value) == ESP_OK)
        {
            settings.metric_units = value != 0U;
        }
        if ((nvs_get_u8(handle, "vt_num", &value) == ESP_OK) && (value >= 1U) && (value <= 32U))
        {
            settings.terminal_number = value;
        }
        std::uint16_t data_mask_size = 0;
        if ((nvs_get_u16(handle, "mask_size", &data_mask_size) == ESP_OK) &&
            ((data_mask_size == 480U) || (data_mask_size == 600U)))
        {
            settings.data_mask_size = data_mask_size;
        }
        if ((nvs_get_u8(handle, "ctrl_vol", &value) == ESP_OK) && (value <= 100U))
        {
            settings.control_volume = value;
        }
        if ((nvs_get_u8(handle, "alarm_vol", &value) == ESP_OK) && (value <= 100U))
        {
            settings.alarm_volume = value;
        }
        if (nvs_get_u8(handle, "audio_mute", &value) == ESP_OK)
        {
            settings.audio_muted = value != 0U;
        }
        if (nvs_get_u8(handle, "aux_o", &value) == ESP_OK)
        {
            settings.aux_o_legacy = value != 0U;
        }
        nvs_close(handle);
    }
    vt_scene_renderer::set_terminal_settings(settings);
}

bool save_terminal_settings()
{
    const auto settings = vt_scene_renderer::get_terminal_settings();
    nvs_handle_t handle = 0;
    if (nvs_open(TERMINAL_SETTINGS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    {
        return false;
    }
    esp_err_t result = nvs_set_u8(handle, "language", static_cast<std::uint8_t>(settings.language));
    if (result == ESP_OK)
    {
        result = nvs_set_u8(handle, "date", static_cast<std::uint8_t>(settings.date_format));
    }
    if (result == ESP_OK)
    {
        result = nvs_set_u8(handle, "time24", settings.twenty_four_hour ? 1U : 0U);
    }
    if (result == ESP_OK)
    {
        result = nvs_set_u8(handle, "decimal", settings.decimal_comma ? 1U : 0U);
    }
    if (result == ESP_OK)
    {
        result = nvs_set_u8(handle, "metric", settings.metric_units ? 1U : 0U);
    }
    if (result == ESP_OK)
    {
        result = nvs_set_u8(handle, "vt_num", settings.terminal_number);
    }
    if (result == ESP_OK)
    {
        result = nvs_set_u16(handle, "mask_size", settings.data_mask_size);
    }
    if (result == ESP_OK)
    {
        result = nvs_set_u8(handle, "ctrl_vol", settings.control_volume);
    }
    if (result == ESP_OK)
    {
        result = nvs_set_u8(handle, "alarm_vol", settings.alarm_volume);
    }
    if (result == ESP_OK)
    {
        result = nvs_set_u8(handle, "audio_mute", settings.audio_muted ? 1U : 0U);
    }
    if (result == ESP_OK)
    {
        result = nvs_set_u8(handle, "aux_o", settings.aux_o_legacy ? 1U : 0U);
    }
    if (result == ESP_OK)
    {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result == ESP_OK;
}

void begin_object_pool_progress(std::uint32_t expected_bytes)
{
    expected_object_pool_bytes = expected_bytes;
    received_object_pool_bytes = 0;
    last_object_pool_percent = 0;
    last_object_pool_progress_timestamp_ms = 0;
    vt_view::set_object_pool_download_progress(true, 0);
}

void finish_object_pool_progress()
{
    expected_object_pool_bytes = 0;
    received_object_pool_bytes = 0;
    last_object_pool_percent = 100;
    vt_view::set_object_pool_download_progress(true, 100);
}

void update_object_pool_progress()
{
    if (expected_object_pool_bytes == 0)
    {
        return;
    }

    const std::uint32_t now = isobus::SystemTiming::get_timestamp_ms();
    if (!isobus::SystemTiming::time_expired_ms(last_object_pool_progress_timestamp_ms, OBJECT_POOL_PROGRESS_UPDATE_MS))
    {
        return;
    }
    last_object_pool_progress_timestamp_ms = now;

    for (const auto &session : isobus::CANNetworkManager::CANNetwork.get_active_transport_protocol_sessions(0))
    {
        if ((session->get_direction() != isobus::TransportProtocolSessionBase::Direction::Receive) ||
            (session->get_parameter_group_number() != static_cast<std::uint32_t>(isobus::CANLibParameterGroupNumber::ECUtoVirtualTerminal)) ||
            (session->get_data().size() < 1) ||
            (session->get_data().get_byte(0) != 0x11))
        {
            continue;
        }

        std::uint32_t active_component_bytes = session->get_total_bytes_transferred();
        if (active_component_bytes > 0)
        {
            --active_component_bytes; // The transfer command byte is not object-pool data.
        }
        const std::uint64_t received_bytes =
            static_cast<std::uint64_t>(received_object_pool_bytes) + active_component_bytes;
        const auto percent = static_cast<std::uint8_t>(std::min<std::uint64_t>(
            99,
            (received_bytes * 100U) / expected_object_pool_bytes));
        if (percent != last_object_pool_percent)
        {
            last_object_pool_percent = percent;
            vt_view::set_object_pool_download_progress(true, percent);
        }
        break;
    }
}

void service_key_still_held()
{
    if (!terminal_server)
    {
        return;
    }

    vt_scene_renderer::HitTarget target;
    std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> working_set;
    {
        std::lock_guard<std::mutex> lock(touch_mutex);
        if ((pressed_target.kind != vt_scene_renderer::HitKind::Button) &&
            (pressed_target.kind != vt_scene_renderer::HitKind::SoftKey))
        {
            return;
        }
        if (!isobus::SystemTiming::time_expired_ms(last_key_held_timestamp_ms,
                                                   KEY_STILL_HELD_INTERVAL_MS))
        {
            return;
        }
        target = pressed_target;
        working_set = rendered_working_set.lock();
        last_key_held_timestamp_ms = isobus::SystemTiming::get_timestamp_ms();
    }

    if (!working_set || !working_set->get_control_function())
    {
        return;
    }

    if (target.kind == vt_scene_renderer::HitKind::Button)
    {
        terminal_server->send_button_activation_message(
            isobus::VirtualTerminalBase::KeyActivationCode::ButtonStillHeld,
            target.object_id,
            target.parent_id,
            target.key_code,
            working_set->get_control_function());
    }
    else
    {
        terminal_server->send_soft_key_activation_message(
            isobus::VirtualTerminalBase::KeyActivationCode::ButtonStillHeld,
            target.object_id,
            target.parent_id,
            target.key_code,
            working_set->get_control_function());
    }
}

void update_virtual_terminal()
{
    if (speed_messages)
    {
        const std::uint32_t now = isobus::SystemTiming::get_timestamp_ms();
        const auto gnss = gnss_um982::get_snapshot();
        const bool velocity_fresh = gnss.position_valid &&
                                    (gnss.last_velocity_ms > 0) &&
                                    ((static_cast<std::int64_t>(now) - gnss.last_velocity_ms) <= GNSS_VELOCITY_TIMEOUT_MS);
        const float speed_mps = velocity_fresh ? std::max(0.0F, gnss.speed_mps) : 0.0F;
        const auto speed_mm_per_second = static_cast<std::uint16_t>(std::clamp(
            std::lround(speed_mps * 1000.0F), 0L, 64255L));

        if ((speed_last_update_ms != 0) && velocity_fresh)
        {
            const std::uint32_t elapsed_ms = std::min<std::uint32_t>(now - speed_last_update_ms, 500U);
            speed_distance_mm = (speed_distance_mm +
                                 (static_cast<std::uint64_t>(speed_mm_per_second) * elapsed_ms) / 1000U) %
                                SPEED_DISTANCE_ROLLOVER_MM;
        }
        speed_last_update_ms = now;

        using Speed = isobus::SpeedMessagesInterface;
        const auto direction = velocity_fresh ? Speed::MachineDirection::Forward
                                              : Speed::MachineDirection::NotAvailable;
        speed_messages->groundBasedSpeedTransmitData.set_machine_speed(speed_mm_per_second);
        speed_messages->groundBasedSpeedTransmitData.set_machine_distance(static_cast<std::uint32_t>(speed_distance_mm));
        speed_messages->groundBasedSpeedTransmitData.set_machine_direction_of_travel(direction);

        speed_messages->machineSelectedSpeedTransmitData.set_machine_speed(speed_mm_per_second);
        speed_messages->machineSelectedSpeedTransmitData.set_machine_distance(static_cast<std::uint32_t>(speed_distance_mm));
        speed_messages->machineSelectedSpeedTransmitData.set_machine_direction_of_travel(direction);
        speed_messages->machineSelectedSpeedTransmitData.set_speed_source(
            velocity_fresh ? Speed::MachineSelectedSpeedData::SpeedSource::NavigationBasedSpeed
                           : Speed::MachineSelectedSpeedData::SpeedSource::NotAvailable);
        speed_messages->machineSelectedSpeedTransmitData.set_exit_reason_code(
            static_cast<std::uint8_t>(velocity_fresh ? Speed::MachineSelectedSpeedData::ExitReasonCode::NoReasonAllClear
                                                     : Speed::MachineSelectedSpeedData::ExitReasonCode::NotAvailable));
        speed_messages->machineSelectedSpeedTransmitData.set_limit_status(
            velocity_fresh ? Speed::MachineSelectedSpeedData::LimitStatus::NotLimited
                           : Speed::MachineSelectedSpeedData::LimitStatus::NotAvailable);
        speed_messages->update();

        if ((velocity_fresh != speed_was_valid) ||
            isobus::SystemTiming::time_expired_ms(speed_last_log_ms, SPEED_STATUS_LOG_INTERVAL_MS))
        {
            ESP_LOGI(TAG, "ISOBUS GNSS speed TX: %s %.2f km/h (PGN FE49 + F022)",
                     velocity_fresh ? "valid" : "unavailable", speed_mps * 3.6F);
            speed_was_valid = velocity_fresh;
            speed_last_log_ms = now;
        }
    }

    update_object_pool_progress();
    service_key_still_held();

    if (terminal_server != nullptr)
    {
        terminal_server->update_server();

        std::vector<std::pair<std::uint64_t, std::uint8_t>> inactive_machines;
        {
            std::lock_guard<std::mutex> lock(inactive_machine_mutex);
            inactive_machines.swap(pending_inactive_machines);
        }
        for (const auto &[name, address] : inactive_machines)
        {
            if (terminal_server->deactivate_working_set(name))
            {
                {
                    std::lock_guard<std::mutex> lock(render_request_mutex);
                    pending_render_working_set.reset();
                    pending_render_working_sets.clear();
                    post_transition_render_working_set.reset();
                    post_transition_renders_remaining = 0;
                }
                rendered_working_set.reset();
                vt_scene_renderer::close_menu();
                vt_scene_renderer::clear();
                vt_view::set_object_pool_download_progress(false, 0);
                ESP_LOGI(TAG, "Inactive machine SA %u removed from the visible VT area",
                         static_cast<unsigned>(address));
            }
        }
        service_post_transition_renders();
        const std::uint64_t requested_working_set = pending_working_set_selection.exchange(0);
        if ((requested_working_set != 0) && terminal_server->activate_working_set(requested_working_set))
        {
            render_working_set(terminal_server->get_ui_active_working_set());
        }
        const std::uint32_t now = isobus::SystemTiming::get_timestamp_ms();
        if (vt_scene_renderer::is_menu_open() &&
            isobus::SystemTiming::time_expired_ms(last_terminal_menu_refresh_ms, 1000))
        {
            last_terminal_menu_refresh_ms = now;
            render_working_set(terminal_server->get_ui_active_working_set());
        }
    }
}

void working_set_render_task(void *)
{
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Dynamic soft-key masks are commonly updated with a short burst of
        // ObjectPointer commands. Wait for the burst to settle so one complete
        // frame is shown instead of several partially updated frames.
        const TickType_t burst_started = xTaskGetTickCount();
        while (((xTaskGetTickCount() - burst_started) < pdMS_TO_TICKS(50)) &&
               (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RENDER_COALESCE_DELAY_MS)) > 0))
        {
        }

        std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> working_set;
        std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> working_sets;
        std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> auxiliary_working_sets;
        {
            std::lock_guard<std::mutex> lock(render_request_mutex);
            working_set = std::move(pending_render_working_set);
            working_sets = std::move(pending_render_working_sets);
            auxiliary_working_sets = std::move(pending_auxiliary_working_sets);
        }

        const bool render_requested = working_set || vt_scene_renderer::is_menu_open();
        bool rendered = render_requested && vt_scene_renderer::render(working_set, working_sets, auxiliary_working_sets);
        if (render_requested && !rendered)
        {
            vTaskDelay(pdMS_TO_TICKS(RENDER_RETRY_DELAY_MS));
            rendered = vt_scene_renderer::render(working_set, working_sets, auxiliary_working_sets);
        }
        if (rendered)
        {
            std::lock_guard<std::mutex> lock(touch_mutex);
            rendered_working_set = working_set;
        }
    }
}

bool render_working_set(const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set)
{
    if ((!working_set && !vt_scene_renderer::is_menu_open()) ||
        (working_set_render_task_handle == nullptr))
    {
        return false;
    }

    // CAN repaint callbacks already hold the working-set list lock. A touch
    // request must not take render_request_mutex before requesting that list,
    // otherwise each thread can wait for the lock held by the other.
    std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> working_sets;
    std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> auxiliary_working_sets;
    if (terminal_server)
    {
        auxiliary_working_sets = terminal_server->get_all_working_sets();
        aux_n::refresh_catalog(working_set, auxiliary_working_sets);
        working_sets = terminal_server->get_renderable_working_sets();
    }
    {
        std::lock_guard<std::mutex> lock(render_request_mutex);
        pending_render_working_set = working_set;
        pending_render_working_sets = std::move(working_sets);
        pending_auxiliary_working_sets = std::move(auxiliary_working_sets);
    }
    xTaskNotifyGive(working_set_render_task_handle);
    return true;
}

isobus::NAME make_terminal_name()
{
    isobus::NAME name(0);
    name.set_arbitrary_address_capable(true);
    name.set_industry_group(static_cast<std::uint8_t>(isobus::NAME::IndustryGroup::AgriculturalAndForestryEquipment));
    name.set_device_class(0);
    name.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::VirtualTerminal));
    name.set_device_class_instance(0);
    name.set_function_instance(static_cast<std::uint8_t>(vt_scene_renderer::get_terminal_settings().terminal_number - 1U));
    name.set_ecu_instance(0);
    name.set_manufacturer_code(CONFIG_ISOVT_MANUFACTURER_CODE);
    name.set_identity_number(CONFIG_ISOVT_IDENTITY_NUMBER);
    return name;
}

isobus::NAME make_speed_provider_name()
{
    isobus::NAME name(0);
    name.set_arbitrary_address_capable(true);
    name.set_industry_group(static_cast<std::uint8_t>(isobus::NAME::IndustryGroup::AgriculturalAndForestryEquipment));
    name.set_device_class(static_cast<std::uint8_t>(isobus::NAME::DeviceClass::NonSpecific));
    name.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::TractorECU));
    name.set_device_class_instance(0);
    name.set_function_instance(0);
    name.set_ecu_instance(0);
    name.set_manufacturer_code(CONFIG_ISOVT_MANUFACTURER_CODE);
    name.set_identity_number(CONFIG_ISOVT_IDENTITY_NUMBER);
    return name;
}

bool configure_language_and_units(bool broadcast)
{
    if (!terminal_server)
    {
        return false;
    }

    const auto settings = vt_scene_renderer::get_terminal_settings();
    auto &language = terminal_server->get_language_command_interface();
    using Language = isobus::LanguageCommandInterface;
    const char *language_code = "pl";
    const char *country_code = "PL";
    switch (settings.language)
    {
        case vt_scene_renderer::TerminalLanguage::English:
            language_code = "en";
            country_code = "GB";
            break;
        case vt_scene_renderer::TerminalLanguage::German:
            language_code = "de";
            country_code = "DE";
            break;
        case vt_scene_renderer::TerminalLanguage::French:
            language_code = "fr";
            country_code = "FR";
            break;
        case vt_scene_renderer::TerminalLanguage::Hungarian:
            language_code = "hu";
            country_code = "HU";
            break;
        case vt_scene_renderer::TerminalLanguage::Dutch:
            language_code = "nl";
            country_code = "NL";
            break;
        case vt_scene_renderer::TerminalLanguage::Russian:
            language_code = "ru";
            country_code = "RU";
            break;
        case vt_scene_renderer::TerminalLanguage::Lithuanian:
            language_code = "lt";
            country_code = "LT";
            break;
        default:
            break;
    }
    language.set_language_code(language_code);
    language.set_country_code(country_code);
    language.set_commanded_decimal_symbol(settings.decimal_comma ? Language::DecimalSymbols::Comma : Language::DecimalSymbols::Point);
    language.set_commanded_time_format(settings.twenty_four_hour ? Language::TimeFormats::TwentyFourHour : Language::TimeFormats::TwelveHourAmPm);
    Language::DateFormats date_format = Language::DateFormats::ddmmyyyy;
    if (settings.date_format == vt_scene_renderer::TerminalDateFormat::MonthDayYear)
    {
        date_format = Language::DateFormats::mmddyyyy;
    }
    else if (settings.date_format == vt_scene_renderer::TerminalDateFormat::YearMonthDay)
    {
        date_format = Language::DateFormats::yyyymmdd;
    }
    language.set_commanded_date_format(date_format);
    language.set_commanded_distance_units(settings.metric_units ? Language::DistanceUnits::Metric : Language::DistanceUnits::ImperialUS);
    language.set_commanded_area_units(settings.metric_units ? Language::AreaUnits::Metric : Language::AreaUnits::ImperialUS);
    language.set_commanded_volume_units(settings.metric_units ? Language::VolumeUnits::Metric : Language::VolumeUnits::Imperial);
    language.set_commanded_mass_units(settings.metric_units ? Language::MassUnits::Metric : Language::MassUnits::Imperial);
    language.set_commanded_temperature_units(settings.metric_units ? Language::TemperatureUnits::Metric : Language::TemperatureUnits::ImperialUS);
    language.set_commanded_pressure_units(settings.metric_units ? Language::PressureUnits::Metric : Language::PressureUnits::ImperialUS);
    language.set_commanded_force_units(settings.metric_units ? Language::ForceUnits::Metric : Language::ForceUnits::ImperialUS);
    language.set_commanded_generic_units(settings.metric_units ? Language::UnitSystem::Metric : Language::UnitSystem::Imperial);
    return !broadcast || language.send_language_command();
}

bool announce_language_for_working_set(
    const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set)
{
    static std::vector<std::weak_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> announced_working_sets;
    announced_working_sets.erase(
        std::remove_if(announced_working_sets.begin(),
                       announced_working_sets.end(),
                       [](const auto &entry) { return entry.expired(); }),
        announced_working_sets.end());
    for (const auto &entry : announced_working_sets)
    {
        if (entry.lock() == working_set)
        {
            return true;
        }
    }
    if (!configure_language_and_units(true))
    {
        return false;
    }
    announced_working_sets.emplace_back(working_set);
    ESP_LOGI(TAG, "Language and units announced for working set 0x%016llx",
             static_cast<unsigned long long>(client_name(working_set)));
    return true;
}

void connect_view_events(EspVirtualTerminalServer &server)
{
    server.get_on_repaint_event_dispatcher().add_listener([&server](const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set) {
        server.ensure_active_working_set(working_set);
        if (!announce_language_for_working_set(working_set))
        {
            ESP_LOGW(TAG, "Could not queue language command for working set 0x%016llx; will retry",
                     static_cast<unsigned long long>(client_name(working_set)));
        }
        render_working_set(server.get_ui_active_working_set());
        // A dynamic soft-key mask may arrive as a burst of ObjectPointer
        // changes. Keep a few trailing frames scheduled from the last change
        // so the completed mask is always presented.
        schedule_post_transition_renders(server.get_ui_active_working_set());
        vt_view::on_repaint_requested(client_name(working_set));
    });
    server.get_on_change_active_mask_event_dispatcher().add_listener([&server](const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set,
                                                                         std::uint16_t previous_mask,
                                                                         std::uint16_t active_mask) {
        server.sync_active_masks(working_set);
        if (server.get_ui_active_working_set() == working_set)
        {
            server.announce_active_layout_visibility(working_set);
            render_working_set(working_set);
            schedule_post_transition_renders(working_set);
        }
        vt_view::on_active_mask_changed(client_name(working_set), previous_mask, active_mask);
    });
    server.get_on_change_active_softkey_mask_event_dispatcher().add_listener([&server](const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set,
                                                                                 std::uint16_t previous_mask,
                                                                                 std::uint16_t active_mask) {
        server.sync_active_masks(working_set);
        if (server.get_ui_active_working_set() == working_set)
        {
            server.announce_active_layout_visibility(working_set);
            render_working_set(working_set);
            schedule_post_transition_renders(working_set);
        }
        vt_view::on_soft_key_mask_changed(client_name(working_set), previous_mask, active_mask);
    });
    server.get_on_focus_object_event_dispatcher().add_listener([](const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set,
                                                                    std::uint16_t object_id,
                                                                    bool input_open) {
        vt_view::on_focus_changed(client_name(working_set), object_id, input_open);
    });
}
#endif
}

namespace isovt
{
void on_machine_inactive(std::uint64_t name, std::uint8_t address)
{
#if ISOVT_ENABLE_AGISOSTACK
    std::lock_guard<std::mutex> lock(inactive_machine_mutex);
    const auto duplicate = std::find_if(
        pending_inactive_machines.begin(),
        pending_inactive_machines.end(),
        [name](const auto &entry) { return entry.first == name; });
    if (duplicate == pending_inactive_machines.end())
    {
        pending_inactive_machines.emplace_back(name, address);
    }
#else
    (void)name;
    (void)address;
#endif
}

bool start_local_ui()
{
#if ISOVT_ENABLE_AGISOSTACK
    if (working_set_render_task_handle == nullptr)
    {
        load_terminal_settings();
        const BaseType_t task_result = xTaskCreatePinnedToCore(
            working_set_render_task,
            "vt_scene",
            12288,
            nullptr,
            3,
            &working_set_render_task_handle,
            0);
        if (task_result != pdPASS)
        {
            working_set_render_task_handle = nullptr;
            ESP_LOGE(TAG, "Could not create VT scene render task");
            vt_view::set_status("VT renderer startup failed");
            return false;
        }
    }
    return true;
#else
    return false;
#endif
}

bool start()
{
#if ISOVT_ENABLE_AGISOSTACK
    if (terminal_server)
    {
        return true;
    }
    if (!start_local_ui())
    {
        return false;
    }

    isobus::CANStackLogger::set_can_stack_logger_sink(&stack_logger);
    // Stack logging is synchronous on ESP-IDF. Keep routine protocol traffic off
    // the timing-sensitive update path; warnings and errors remain visible.
    isobus::CANStackLogger::set_log_level(isobus::CANStackLogger::LoggingLevel::Warning);

    terminal_control_function = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(
        make_terminal_name(), 0, VT_PREFERRED_ADDRESS);
    if (!terminal_control_function)
    {
        ESP_LOGE(TAG, "Could not create ISOBUS Virtual Terminal control function");
        vt_view::set_status("ISOBUS address-claim setup failed");
        return false;
    }

    terminal_server = std::make_shared<EspVirtualTerminalServer>(terminal_control_function);
    connect_view_events(*terminal_server);
    terminal_server->initialize();
    configure_language_and_units(false);
    terminal_server->get_language_command_interface().initialize();

    speed_control_function = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(
        make_speed_provider_name(), 0, SPEED_PROVIDER_PREFERRED_ADDRESS);
    if (speed_control_function)
    {
        // Send measured ground speed and the selected navigation-based speed.
        // The command PGN remains disabled: this bridge never commands vehicle motion.
        speed_messages = std::make_unique<isobus::SpeedMessagesInterface>(
            speed_control_function, true, false, true, false);
        speed_messages->initialize();
        ESP_LOGI(TAG, "GNSS ISOBUS speed provider ready, preferred address=0x%02x",
                 SPEED_PROVIDER_PREFERRED_ADDRESS);
    }
    else
    {
        ESP_LOGW(TAG, "Could not create GNSS ISOBUS speed provider");
    }
    if (!vt_update_callback_registered)
    {
        isobus::CANHardwareInterface::get_periodic_update_event_dispatcher().add_listener(update_virtual_terminal);
        vt_update_callback_registered = true;
    }
    if (!aux_n::start(terminal_control_function))
    {
        ESP_LOGE(TAG, "Could not initialize AUX-N manager");
        return false;
    }

    ESP_LOGI(TAG, "Virtual Terminal initialized, NAME=0x%016llx address=0x%02x",
             static_cast<unsigned long long>(terminal_control_function->get_NAME().get_full_name()),
             VT_PREFERRED_ADDRESS);
    vt_view::set_status("ISOBUS VT and AUX-N assignment service ready");
    return true;
#else
    ESP_LOGW(TAG, "AgIsoStack++ adapter disabled");
    vt_view::set_status("CAN smoke test active; AgIsoStack++ disabled");
    return false;
#endif
}

#if ISOVT_ENABLE_AGISOSTACK
std::uint32_t referenced_number_value(
    const std::shared_ptr<isobus::VTObject> &object,
    const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set,
    std::uint32_t fallback)
{
    const auto referenced = std::static_pointer_cast<isobus::VTObjectWithVariableReference>(object);
    if (referenced && (referenced->get_variable_reference() != isobus::NULL_OBJECT_ID))
    {
        const auto variable = working_set->get_object_by_id(referenced->get_variable_reference());
        if (variable && (variable->get_object_type() == isobus::VirtualTerminalObjectType::NumberVariable))
        {
            return std::static_pointer_cast<isobus::NumberVariable>(variable)->get_value();
        }
    }
    return fallback;
}

void select_input_object(const std::shared_ptr<isobus::VTObject> &object,
                         const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set)
{
    if (!object || !working_set || !working_set->get_control_function())
    {
        return;
    }
    terminal_server->send_select_input_object_message(object->get_id(), true, true, working_set->get_control_function());
    working_set->set_object_focus(object->get_id());
    terminal_server->process_macro(object,
                                   isobus::EventID::OnInputFieldSelection,
                                   object->get_object_type(),
                                   working_set);
}

void deselect_input_object(const std::shared_ptr<isobus::VTObject> &object,
                           const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set)
{
    if (!object || !working_set || !working_set->get_control_function())
    {
        return;
    }
    terminal_server->send_select_input_object_message(object->get_id(), false, false, working_set->get_control_function());
    working_set->set_object_focus(isobus::NULL_OBJECT_ID);
    terminal_server->process_macro(object,
                                   isobus::EventID::OnInputFieldDeselection,
                                   object->get_object_type(),
                                   working_set);
}

bool is_utf16_le(const std::string &value)
{
    return (value.size() >= 2) &&
           (static_cast<std::uint8_t>(value[0]) == 0xFFU) &&
           (static_cast<std::uint8_t>(value[1]) == 0xFEU);
}

std::uint8_t unicode_to_iso8859_2(std::uint16_t value)
{
    if (value < 0x80U)
    {
        return static_cast<std::uint8_t>(value);
    }
    switch (value)
    {
        case 0x0104: return 0xA1;
        case 0x0141: return 0xA3;
        case 0x015A: return 0xA6;
        case 0x0179: return 0xAC;
        case 0x017B: return 0xAF;
        case 0x0105: return 0xB1;
        case 0x0142: return 0xB3;
        case 0x015B: return 0xB6;
        case 0x017A: return 0xBC;
        case 0x017C: return 0xBF;
        case 0x0106: return 0xC6;
        case 0x0118: return 0xCA;
        case 0x0143: return 0xD1;
        case 0x00D3: return 0xD3;
        case 0x0107: return 0xE6;
        case 0x0119: return 0xEA;
        case 0x0144: return 0xF1;
        case 0x00F3: return 0xF3;
        default: return static_cast<std::uint8_t>('?');
    }
}

std::uint16_t iso8859_2_to_unicode(std::uint8_t value)
{
    if (value < 0x80U)
    {
        return value;
    }
    switch (value)
    {
        case 0xA1: return 0x0104;
        case 0xA3: return 0x0141;
        case 0xA6: return 0x015A;
        case 0xAC: return 0x0179;
        case 0xAF: return 0x017B;
        case 0xB1: return 0x0105;
        case 0xB3: return 0x0142;
        case 0xB6: return 0x015B;
        case 0xBC: return 0x017A;
        case 0xBF: return 0x017C;
        case 0xC6: return 0x0106;
        case 0xCA: return 0x0118;
        case 0xD1: return 0x0143;
        case 0xD3: return 0x00D3;
        case 0xE6: return 0x0107;
        case 0xEA: return 0x0119;
        case 0xF1: return 0x0144;
        case 0xF3: return 0x00F3;
        default: return static_cast<std::uint16_t>('?');
    }
}

std::string decode_utf16_editor_value(const std::string &value)
{
    if (!is_utf16_le(value))
    {
        return value;
    }
    std::string decoded;
    for (std::size_t index = 2; (index + 1) < value.size(); index += 2)
    {
        const auto codepoint = static_cast<std::uint16_t>(
            static_cast<std::uint8_t>(value[index]) |
            (static_cast<std::uint16_t>(static_cast<std::uint8_t>(value[index + 1])) << 8));
        if (codepoint == 0U)
        {
            break;
        }
        decoded.push_back(static_cast<char>(unicode_to_iso8859_2(codepoint)));
    }
    while (!decoded.empty() && (decoded.back() == ' '))
    {
        decoded.pop_back();
    }
    return decoded;
}

std::string encode_utf16_editor_value(const std::string &value, std::size_t target_size)
{
    std::string encoded{ static_cast<char>(0xFF), static_cast<char>(0xFE) };
    for (const auto character : value)
    {
        const std::uint16_t codepoint = iso8859_2_to_unicode(static_cast<std::uint8_t>(character));
        encoded.push_back(static_cast<char>(codepoint & 0xFFU));
        encoded.push_back(static_cast<char>((codepoint >> 8) & 0xFFU));
    }
    while ((encoded.size() + 1) < target_size)
    {
        encoded.push_back(' ');
        encoded.push_back('\0');
    }
    if (encoded.size() > target_size)
    {
        encoded.resize(target_size & ~std::size_t{ 1 });
    }
    return encoded;
}

vt_scene_renderer::EditorTextEncoding editor_text_encoding(
    const std::shared_ptr<isobus::InputString> &input,
    const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set,
    const std::string &value)
{
    if (is_utf16_le(value))
    {
        return vt_scene_renderer::EditorTextEncoding::ISO8859_2;
    }
    const auto attributes = working_set->get_object_by_id(input->get_font_attributes());
    if (!attributes || (attributes->get_object_type() != isobus::VirtualTerminalObjectType::FontAttributes))
    {
        return vt_scene_renderer::EditorTextEncoding::ISO8859_1;
    }
    switch (std::static_pointer_cast<isobus::FontAttributes>(attributes)->get_type())
    {
        case isobus::FontAttributes::FontType::ISO8859_15: return vt_scene_renderer::EditorTextEncoding::ISO8859_15;
        case isobus::FontAttributes::FontType::ISO8859_2: return vt_scene_renderer::EditorTextEncoding::ISO8859_2;
        case isobus::FontAttributes::FontType::ISO8859_4: return vt_scene_renderer::EditorTextEncoding::ISO8859_4;
        case isobus::FontAttributes::FontType::ISO8859_5: return vt_scene_renderer::EditorTextEncoding::ISO8859_5;
        case isobus::FontAttributes::FontType::ISO8859_7: return vt_scene_renderer::EditorTextEncoding::ISO8859_7;
        default: return vt_scene_renderer::EditorTextEncoding::ISO8859_1;
    }
}

bool open_input_editor(const std::shared_ptr<isobus::VTObject> &object,
                       const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set)
{
    if (!object || !working_set)
    {
        return false;
    }
    using Type = isobus::VirtualTerminalObjectType;
    switch (object->get_object_type())
    {
        case Type::InputNumber:
        {
            const auto input = std::static_pointer_cast<isobus::InputNumber>(object);
            const std::uint32_t raw_value = referenced_number_value(object, working_set, input->get_value());
            char value[48];
            std::snprintf(value,
                          sizeof(value),
                          "%.*f",
                          std::min<int>(input->get_number_of_decimals(), 6),
                          (static_cast<double>(raw_value) + input->get_offset()) * input->get_scale());
            vt_scene_renderer::open_input_editor(vt_scene_renderer::InputEditorKind::Number,
                                                 object->get_id(),
                                                 value,
                                                 24);
            break;
        }
        case Type::InputString:
        {
            const auto input = std::static_pointer_cast<isobus::InputString>(object);
            std::string value = input->get_value();
            if (input->get_variable_reference() != isobus::NULL_OBJECT_ID)
            {
                const auto variable = working_set->get_object_by_id(input->get_variable_reference());
                if (variable && (variable->get_object_type() == Type::StringVariable))
                {
                    value = std::static_pointer_cast<isobus::StringVariable>(variable)->get_value();
                }
            }
            const auto encoding = editor_text_encoding(input, working_set, value);
            value = decode_utf16_editor_value(value);
            vt_scene_renderer::open_input_editor(vt_scene_renderer::InputEditorKind::Text,
                                                 object->get_id(),
                                                 value,
                                                 value.empty() ? 32 : value.size(),
                                                 {},
                                                 0,
                                                 encoding);
            break;
        }
        case Type::InputList:
        {
            const auto input = std::static_pointer_cast<isobus::InputList>(object);
            std::vector<vt_scene_renderer::EditorListItem> items;
            items.reserve(input->get_number_children());
            for (std::uint16_t index = 0; index < input->get_number_children(); ++index)
            {
                const auto child = working_set->get_object_by_id(input->get_child_id(index));
                vt_scene_renderer::EditorListItem item;
                item.object_id = child ? child->get_id() : isobus::NULL_OBJECT_ID;
                if (child && (child->get_object_type() == Type::OutputString))
                {
                    const auto output = std::static_pointer_cast<isobus::OutputString>(child);
                    item.label = output->displayed_value(working_set->get_object_tree());
                    item.object_id = isobus::NULL_OBJECT_ID;
                    const auto attributes = working_set->get_object_by_id(output->get_font_attributes());
                    if (attributes && (attributes->get_object_type() == Type::FontAttributes))
                    {
                        switch (std::static_pointer_cast<isobus::FontAttributes>(attributes)->get_type())
                        {
                            case isobus::FontAttributes::FontType::ISO8859_15:
                                item.encoding = vt_scene_renderer::EditorTextEncoding::ISO8859_15;
                                break;
                            case isobus::FontAttributes::FontType::ISO8859_2:
                                item.encoding = vt_scene_renderer::EditorTextEncoding::ISO8859_2;
                                break;
                            case isobus::FontAttributes::FontType::ISO8859_4:
                                item.encoding = vt_scene_renderer::EditorTextEncoding::ISO8859_4;
                                break;
                            case isobus::FontAttributes::FontType::ISO8859_5:
                                item.encoding = vt_scene_renderer::EditorTextEncoding::ISO8859_5;
                                break;
                            case isobus::FontAttributes::FontType::ISO8859_7:
                                item.encoding = vt_scene_renderer::EditorTextEncoding::ISO8859_7;
                                break;
                            default:
                                break;
                        }
                    }
                }
                else if (child && (child->get_object_type() == Type::OutputNumber))
                {
                    const auto number = std::static_pointer_cast<isobus::OutputNumber>(child);
                    char label[40];
                    std::snprintf(label,
                                  sizeof(label),
                                  "%.*f",
                                  std::min<int>(number->get_number_of_decimals(), 4),
                                  (static_cast<double>(number->get_value()) + number->get_offset()) * number->get_scale());
                    item.label = label;
                    item.object_id = isobus::NULL_OBJECT_ID;
                }
                else if (!child)
                {
                    char label[40];
                    std::snprintf(label, sizeof(label), "POZYCJA %u", static_cast<unsigned>(index + 1U));
                    item.label = label;
                }
                items.push_back(std::move(item));
            }
            const auto selected = static_cast<std::uint16_t>(referenced_number_value(object,
                                                                                      working_set,
                                                                                      input->get_value()));
            vt_scene_renderer::open_input_editor(vt_scene_renderer::InputEditorKind::List,
                                                 object->get_id(),
                                                 "",
                                                 1,
                                                 items,
                                                 selected);
            break;
        }
        default:
            return false;
    }
    select_input_object(object, working_set);
    render_working_set(working_set);
    return true;
}

bool commit_input_editor(const vt_scene_renderer::InputEditorResult &result,
                         const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set)
{
    if (!working_set || !terminal_server)
    {
        return false;
    }
    const auto object = working_set->get_object_by_id(result.object_id);
    if (!object)
    {
        return false;
    }
    const auto destination = working_set->get_control_function();
    bool changed = false;
    if (result.committed)
    {
        terminal_server->process_macro(object, isobus::EventID::OnEntryOfAValue, object->get_object_type(), working_set);
        if (result.kind == vt_scene_renderer::InputEditorKind::Number)
        {
            const auto input = std::static_pointer_cast<isobus::InputNumber>(object);
            std::string normalized = result.value;
            std::replace(normalized.begin(), normalized.end(), ',', '.');
            char *end = nullptr;
            const double displayed = std::strtod(normalized.c_str(), &end);
            if ((end != normalized.c_str()) && std::isfinite(displayed) && (std::fabs(input->get_scale()) > 1.0e-12))
            {
                const auto calculated = static_cast<std::int64_t>(std::llround(displayed / input->get_scale() - input->get_offset()));
                const auto raw = static_cast<std::uint32_t>(std::clamp<std::int64_t>(calculated,
                                                                                   input->get_minimum_value(),
                                                                                   input->get_maximum_value()));
                auto value_object = object;
                std::uint32_t previous = input->get_value();
                if (input->get_variable_reference() != isobus::NULL_OBJECT_ID)
                {
                    const auto variable = working_set->get_object_by_id(input->get_variable_reference());
                    if (variable && (variable->get_object_type() == isobus::VirtualTerminalObjectType::NumberVariable))
                    {
                        value_object = variable;
                        const auto number_variable = std::static_pointer_cast<isobus::NumberVariable>(variable);
                        previous = number_variable->get_value();
                        number_variable->set_value(raw);
                    }
                }
                if (value_object == object)
                {
                    input->set_value(raw);
                }
                changed = previous != raw;
                terminal_server->send_change_numeric_value_message(value_object->get_id(), raw, destination);
                terminal_server->process_macro(value_object, isobus::EventID::OnChangeValue, value_object->get_object_type(), working_set);
            }
        }
        else if (result.kind == vt_scene_renderer::InputEditorKind::List)
        {
            const auto input = std::static_pointer_cast<isobus::InputList>(object);
            auto value_object = object;
            std::uint32_t previous = input->get_value();
            if (input->get_variable_reference() != isobus::NULL_OBJECT_ID)
            {
                const auto variable = working_set->get_object_by_id(input->get_variable_reference());
                if (variable && (variable->get_object_type() == isobus::VirtualTerminalObjectType::NumberVariable))
                {
                    value_object = variable;
                    const auto number_variable = std::static_pointer_cast<isobus::NumberVariable>(variable);
                    previous = number_variable->get_value();
                    number_variable->set_value(result.selected_index);
                }
            }
            if (value_object == object)
            {
                input->set_value(static_cast<std::uint8_t>(result.selected_index));
            }
            changed = previous != result.selected_index;
            terminal_server->send_change_numeric_value_message(value_object->get_id(), result.selected_index, destination);
            terminal_server->process_macro(value_object, isobus::EventID::OnChangeValue, value_object->get_object_type(), working_set);
        }
        else if (result.kind == vt_scene_renderer::InputEditorKind::Text)
        {
            const auto input = std::static_pointer_cast<isobus::InputString>(object);
            auto value_object = object;
            std::string previous = input->get_value();
            if (input->get_variable_reference() != isobus::NULL_OBJECT_ID)
            {
                const auto variable = working_set->get_object_by_id(input->get_variable_reference());
                if (variable && (variable->get_object_type() == isobus::VirtualTerminalObjectType::StringVariable))
                {
                    value_object = variable;
                    const auto string_variable = std::static_pointer_cast<isobus::StringVariable>(variable);
                    previous = string_variable->get_value();
                }
            }
            std::string value = is_utf16_le(previous) ?
                                    encode_utf16_editor_value(result.value, previous.size()) :
                                    result.value;
            if (!is_utf16_le(previous))
            {
                value.resize(previous.size(), ' ');
            }
            if (value_object == object)
            {
                input->set_value(value);
            }
            else
            {
                std::static_pointer_cast<isobus::StringVariable>(value_object)->set_value(value);
            }
            changed = previous != value;
            terminal_server->send_change_string_value_message(value_object->get_id(), value, destination);
            terminal_server->process_macro(value_object, isobus::EventID::OnChangeValue, value_object->get_object_type(), working_set);
        }
        if (changed)
        {
            terminal_server->process_macro(object, isobus::EventID::OnEntryOfANewValue, object->get_object_type(), working_set);
        }
    }
    deselect_input_object(object, working_set);
    render_working_set(working_set);
    return result.committed;
}
#endif

bool on_touch_pressed(std::uint16_t x, std::uint16_t y)
{
#if ISOVT_ENABLE_AGISOSTACK
    auto target = vt_scene_renderer::hit_test(x, y);
    if ((target.kind == vt_scene_renderer::HitKind::None) &&
        vt_view::is_idle_menu_touch(x, y))
    {
        target.kind = vt_scene_renderer::HitKind::Terminal;
        target.terminal_action = vt_scene_renderer::TerminalAction::ToggleMenu;
    }
    if (target.kind == vt_scene_renderer::HitKind::Editor)
    {
        std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> working_set;
        {
            std::lock_guard<std::mutex> lock(touch_mutex);
            working_set = rendered_working_set.lock();
            pressed_target = {};
        }
        const auto result = vt_scene_renderer::apply_editor_action(target.editor_action,
                                                                   target.editor_character,
                                                                   target.editor_index);
        if (result.committed || result.cancelled)
        {
            commit_input_editor(result, working_set);
        }
        else
        {
            render_working_set(working_set);
        }
        return result.handled;
    }
    if (target.kind == vt_scene_renderer::HitKind::None)
    {
        return false;
    }
    if (target.kind == vt_scene_renderer::HitKind::Terminal)
    {
        const auto working_set = terminal_server ? terminal_server->get_ui_active_working_set() : nullptr;
        const bool menu_was_open = vt_scene_renderer::is_menu_open();
        bool result = true;
        switch (target.terminal_action)
        {
            case vt_scene_renderer::TerminalAction::CycleLanguage:
            case vt_scene_renderer::TerminalAction::CycleDateFormat:
            case vt_scene_renderer::TerminalAction::ToggleTimeFormat:
            case vt_scene_renderer::TerminalAction::ToggleDecimalSymbol:
            case vt_scene_renderer::TerminalAction::ToggleUnits:
            {
                vt_scene_renderer::apply_terminal_action(target.terminal_action);
                const bool saved = save_terminal_settings();
                result = !terminal_server || configure_language_and_units(true);
                vt_scene_renderer::set_notice((result && saved) ? "USTAWIENIA ZAPISANE I WYSLANE DO SIECI" :
                                                                    "USTAWIENIE ZMIENIONE, BLAD ZAPISU LUB WYSYLANIA");
                break;
            }
            case vt_scene_renderer::TerminalAction::CycleTerminalNumber:
                vt_scene_renderer::apply_terminal_action(target.terminal_action);
                result = save_terminal_settings();
                vt_scene_renderer::set_notice(result ? "NUMER VT ZAPISANY, URUCHOM TERMINAL PONOWNIE" :
                                                        "NUMER ZMIENIONY, ALE ZAPIS NIE UDAL SIE");
                break;
            case vt_scene_renderer::TerminalAction::CycleDataMaskSize:
                vt_scene_renderer::apply_terminal_action(target.terminal_action);
                result = save_terminal_settings();
                if (result)
                {
                    result = object_pool_store::erase_all();
                }
                vt_scene_renderer::set_notice(result ? "ROZMIAR ZAPISANY; URUCHOM TERMINAL PONOWNIE" :
                                                        "ROZMIAR ZMIENIONY, ALE ZAPIS LUB USUNIECIE PUL NIE UDALO SIE");
                break;
            case vt_scene_renderer::TerminalAction::DecreaseControlVolume:
            case vt_scene_renderer::TerminalAction::IncreaseControlVolume:
            case vt_scene_renderer::TerminalAction::DecreaseAlarmVolume:
            case vt_scene_renderer::TerminalAction::IncreaseAlarmVolume:
            case vt_scene_renderer::TerminalAction::ToggleAudioMute:
            {
                vt_scene_renderer::apply_terminal_action(target.terminal_action);
                const auto settings = vt_scene_renderer::get_terminal_settings();
                vt_audio::set_control_volume(settings.control_volume);
                vt_audio::set_alarm_volume(settings.alarm_volume);
                vt_audio::set_muted(settings.audio_muted);
                result = save_terminal_settings();
                vt_scene_renderer::set_notice(result ? "USTAWIENIA DZWIEKU ZAPISANE" : "BLAD ZAPISU USTAWIEN DZWIEKU");
                break;
            }
            case vt_scene_renderer::TerminalAction::ToggleAuxOLegacy:
                vt_scene_renderer::apply_terminal_action(target.terminal_action);
                result = save_terminal_settings();
                vt_scene_renderer::set_notice(result ? "TRYB AUX ZAPISANY - URUCHOM TERMINAL PONOWNIE" :
                                                        "NIE UDALO SIE ZAPISAC TRYBU AUX");
                break;
            case vt_scene_renderer::TerminalAction::TestControlAudio:
                vt_audio::play_test_control();
                vt_scene_renderer::set_notice(vt_audio::is_muted() ? "DZWIEK JEST WYCISZONY" : "TEST SYGNALU");
                break;
            case vt_scene_renderer::TerminalAction::TestAlarmAudio:
                vt_audio::play_test_alarm();
                vt_scene_renderer::set_notice(vt_audio::is_muted() ? "DZWIEK JEST WYCISZONY" : "TEST ALARMU");
                break;
            case vt_scene_renderer::TerminalAction::RefreshNetwork:
                result = can_monitor::request_address_claims();
                vt_scene_renderer::set_notice(result ? "WYSZUKIWANIE URZADZEN URUCHOMIONE" : "NIE UDALO SIE ODSWIEZYC SIECI");
                break;
            case vt_scene_renderer::TerminalAction::EraseActivePool:
                result = working_set && object_pool_store::erase_all(client_name(working_set));
                vt_scene_renderer::set_notice(result ? "ZAPISANA PULA AKTYWNEJ MASZYNY USUNIETA" : "BRAK PULI LUB BLAD USUWANIA");
                break;
            case vt_scene_renderer::TerminalAction::EraseAllPools:
                result = object_pool_store::erase_all();
                vt_scene_renderer::set_notice(result ? "WSZYSTKIE ZAPISANE PULE USUNIETE" : "BLAD USUWANIA PUL");
                break;
            case vt_scene_renderer::TerminalAction::ClearAuxAssignments:
                result = working_set && working_set->get_control_function() &&
                         aux_n::unassign_all(working_set->get_control_function(), true);
                vt_scene_renderer::set_notice(result ? "POLECENIE USUNIECIA AUX-N WYSLANE" : "BRAK AKTYWNEJ MASZYNY LUB BLAD AUX-N");
                break;
            case vt_scene_renderer::TerminalAction::UnassignSelectedAux:
                result = aux_n::unassign_selected();
                vt_scene_renderer::set_notice(result ? "USUNIECIE WYBRANEGO PRZYPISANIA WYSLANE" :
                                                        "NAJPIERW WYBIERZ FUNKCJE AUX-N");
                break;
            case vt_scene_renderer::TerminalAction::ToggleAuxLearn:
            {
                const auto aux = aux_n::get_catalog();
                if (aux.learning)
                {
                    aux_n::cancel_learning();
                    result = true;
                    vt_scene_renderer::set_notice("UCZENIE AUX-N ANULOWANE");
                }
                else
                {
                    result = aux_n::begin_learning();
                    vt_scene_renderer::set_notice(result ? "PORUSZ WYBRANYM PRZYCISKIEM AUX-N" :
                                                            "WYBIERZ FUNKCJE I PODLACZ AUX-N");
                }
                break;
            }
            default:
                vt_scene_renderer::apply_terminal_action(target.terminal_action);
                break;
        }
        if ((target.terminal_action == vt_scene_renderer::TerminalAction::ToggleMenu) &&
            menu_was_open && !working_set)
        {
            vt_scene_renderer::clear();
            return result;
        }
        render_working_set(working_set);
        return result;
    }
    if ((target.kind == vt_scene_renderer::HitKind::AuxFunction) ||
        (target.kind == vt_scene_renderer::HitKind::AuxInput))
    {
        const auto protocol = target.aux_protocol == static_cast<std::uint8_t>(aux_n::Protocol::LegacyType1) ?
                                  aux_n::Protocol::LegacyType1 : aux_n::Protocol::Type2;
        bool result = false;
        if (target.kind == vt_scene_renderer::HitKind::AuxFunction)
        {
            result = aux_n::select_function(protocol, target.working_set_name, target.object_id);
            vt_scene_renderer::set_notice(result ? "FUNKCJA WYBRANA - DOTKNIJ ZGODNEGO WEJSCIA" :
                                                   "NIE UDALO SIE WYBRAC FUNKCJI AUX");
        }
        else
        {
            result = aux_n::assign_selected(protocol, target.working_set_name, target.object_id);
            vt_scene_renderer::set_notice(result ? "PRZYPISANIE AUX WYSLANE" :
                                                   "WYBIERZ FUNKCJE I ZGODNE WEJSCIE AUX");
        }
        render_working_set(terminal_server ? terminal_server->get_ui_active_working_set() : nullptr);
        return result;
    }
    if (target.kind == vt_scene_renderer::HitKind::WorkingSet)
    {
        if (!terminal_server)
        {
            return false;
        }
        vt_scene_renderer::close_menu();
        pending_working_set_selection.store(target.working_set_name);
        std::lock_guard<std::mutex> lock(touch_mutex);
        pressed_target = target;
        return true;
    }

    std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> working_set;
    {
        std::lock_guard<std::mutex> lock(touch_mutex);
        working_set = rendered_working_set.lock();
        pressed_target = {};
    }
    if (!working_set || !working_set->get_control_function())
    {
        return false;
    }

    const auto destination = working_set->get_control_function();
    bool sent = false;
    switch (target.kind)
    {
        case vt_scene_renderer::HitKind::Button:
            sent = terminal_server->send_button_activation_message(
                isobus::VirtualTerminalBase::KeyActivationCode::ButtonPressedOrLatched,
                target.object_id,
                target.parent_id,
                target.key_code,
                destination);
            if (sent)
            {
                const auto object = working_set->get_object_by_id(target.object_id);
                if (object)
                {
                    terminal_server->process_macro(object,
                                                   isobus::EventID::OnKeyPress,
                                                   object->get_object_type(),
                                                   working_set);
                }
            }
            break;

        case vt_scene_renderer::HitKind::SoftKey:
            sent = terminal_server->send_soft_key_activation_message(
                isobus::VirtualTerminalBase::KeyActivationCode::ButtonPressedOrLatched,
                target.object_id,
                target.parent_id,
                target.key_code,
                destination);
            ESP_LOGD(TAG,
                     "Soft key press: object=%u mask=%u code=%u sent=%u",
                     static_cast<unsigned>(target.object_id),
                     static_cast<unsigned>(target.parent_id),
                     static_cast<unsigned>(target.key_code),
                     sent ? 1U : 0U);
            if (sent)
            {
                const auto object = working_set->get_object_by_id(target.object_id);
                if (object)
                {
                    terminal_server->process_macro(object,
                                                   isobus::EventID::OnKeyPress,
                                                   object->get_object_type(),
                                                   working_set);
                }
            }
            break;

        case vt_scene_renderer::HitKind::Input:
        case vt_scene_renderer::HitKind::InputList:
            sent = open_input_editor(working_set->get_object_by_id(target.object_id), working_set);
            break;

        case vt_scene_renderer::HitKind::InputBoolean:
        {
            const auto object = working_set->get_object_by_id(target.object_id);
            if ((object != nullptr) && (object->get_object_type() == isobus::VirtualTerminalObjectType::InputBoolean))
            {
                const auto input = std::static_pointer_cast<isobus::InputBoolean>(object);
                auto value_object = object;
                std::uint32_t previous_value = input->get_value();
                if (input->get_variable_reference() != isobus::NULL_OBJECT_ID)
                {
                    const auto variable = working_set->get_object_by_id(input->get_variable_reference());
                    if (variable && (variable->get_object_type() == isobus::VirtualTerminalObjectType::NumberVariable))
                    {
                        value_object = variable;
                        previous_value = std::static_pointer_cast<isobus::NumberVariable>(variable)->get_value();
                    }
                }
                const std::uint32_t next_value = previous_value == 0U ? 1U : 0U;
                terminal_server->process_macro(input,
                                               isobus::EventID::OnEntryOfAValue,
                                               input->get_object_type(),
                                               working_set);
                terminal_server->process_macro(input,
                                               isobus::EventID::OnEntryOfANewValue,
                                               input->get_object_type(),
                                               working_set);
                sent = terminal_server->send_change_numeric_value_message(value_object->get_id(), next_value, destination);
                if (sent)
                {
                    if (value_object == object)
                    {
                        input->set_value(static_cast<std::uint8_t>(next_value));
                    }
                    else
                    {
                        std::static_pointer_cast<isobus::NumberVariable>(value_object)->set_value(next_value);
                    }
                    terminal_server->process_macro(value_object,
                                                   isobus::EventID::OnChangeValue,
                                                   value_object->get_object_type(),
                                                   working_set);
                    render_working_set(working_set);
                }
            }
            break;
        }

        case vt_scene_renderer::HitKind::WorkingSet:
        case vt_scene_renderer::HitKind::Terminal:
        case vt_scene_renderer::HitKind::Editor:
        case vt_scene_renderer::HitKind::AuxFunction:
        case vt_scene_renderer::HitKind::AuxInput:
        case vt_scene_renderer::HitKind::None:
            break;
    }

    if (sent && ((target.kind == vt_scene_renderer::HitKind::Button) ||
                 (target.kind == vt_scene_renderer::HitKind::SoftKey)))
    {
        std::lock_guard<std::mutex> lock(touch_mutex);
        pressed_target = target;
        last_key_held_timestamp_ms = isobus::SystemTiming::get_timestamp_ms();
    }
    return sent;
#else
    (void)x;
    (void)y;
    return false;
#endif
}

void on_touch_released()
{
#if ISOVT_ENABLE_AGISOSTACK
    vt_scene_renderer::HitTarget target;
    std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> working_set;
    {
        std::lock_guard<std::mutex> lock(touch_mutex);
        target = pressed_target;
        pressed_target = {};
        last_key_held_timestamp_ms = 0;
        working_set = rendered_working_set.lock();
    }
    if ((target.kind == vt_scene_renderer::HitKind::None) || !terminal_server || !working_set)
    {
        return;
    }

    const auto destination = working_set->get_control_function();
    if (!destination)
    {
        return;
    }
    switch (target.kind)
    {
        case vt_scene_renderer::HitKind::Button:
            terminal_server->send_button_activation_message(
                isobus::VirtualTerminalBase::KeyActivationCode::ButtonUnlatchedOrReleased,
                target.object_id,
                target.parent_id,
                target.key_code,
                destination);
            if (const auto object = working_set->get_object_by_id(target.object_id))
            {
                terminal_server->process_macro(object,
                                               isobus::EventID::OnKeyRelease,
                                               object->get_object_type(),
                                               working_set);
            }
            break;

        case vt_scene_renderer::HitKind::SoftKey:
        {
            const bool sent = terminal_server->send_soft_key_activation_message(
                isobus::VirtualTerminalBase::KeyActivationCode::ButtonUnlatchedOrReleased,
                target.object_id,
                target.parent_id,
                target.key_code,
                destination);
            ESP_LOGD(TAG,
                     "Soft key release: object=%u mask=%u code=%u sent=%u",
                     static_cast<unsigned>(target.object_id),
                     static_cast<unsigned>(target.parent_id),
                     static_cast<unsigned>(target.key_code),
                     sent ? 1U : 0U);
            if (sent)
            {
                if (const auto object = working_set->get_object_by_id(target.object_id))
                {
                    terminal_server->process_macro(object,
                                                   isobus::EventID::OnKeyRelease,
                                                   object->get_object_type(),
                                                   working_set);
                }
            }
            break;
        }

        case vt_scene_renderer::HitKind::Input:
            terminal_server->send_select_input_object_message(target.object_id, false, false, destination);
            break;

        case vt_scene_renderer::HitKind::InputBoolean:
            terminal_server->send_select_input_object_message(target.object_id, false, false, destination);
            break;

        case vt_scene_renderer::HitKind::InputList:
        case vt_scene_renderer::HitKind::Editor:
        case vt_scene_renderer::HitKind::WorkingSet:
        case vt_scene_renderer::HitKind::Terminal:
        case vt_scene_renderer::HitKind::AuxFunction:
        case vt_scene_renderer::HitKind::AuxInput:
        case vt_scene_renderer::HitKind::None:
            break;
    }
#endif
}
}
