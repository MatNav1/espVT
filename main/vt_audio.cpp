#include "vt_audio.hpp"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

#ifndef CONFIG_ISOVT_AUDIO_DEFAULT_VOLUME
#define CONFIG_ISOVT_AUDIO_DEFAULT_VOLUME 70
#endif

#ifndef CONFIG_ISOVT_AUDIO_ALARM_VOLUME
#define CONFIG_ISOVT_AUDIO_ALARM_VOLUME 90
#endif

namespace
{
constexpr char TAG[] = "vt_audio";
constexpr std::uint32_t SAMPLE_RATE = 32000;
constexpr std::size_t FRAMES_PER_BLOCK = 256;
constexpr double TWO_PI = 6.28318530717958647692;

struct SignalCommand
{
    std::uint8_t activations;
    std::uint16_t frequency_hz;
    std::uint16_t duration_ms;
    std::uint16_t off_time_ms;
    bool apply_machine_volume;
};

QueueHandle_t signal_queue = nullptr;
TaskHandle_t audio_task_handle = nullptr;
i2s_chan_handle_t tx_channel = nullptr;
std::atomic<std::uint8_t> control_volume{ CONFIG_ISOVT_AUDIO_DEFAULT_VOLUME };
std::atomic<std::uint8_t> requested_control_volume{ 100U };
std::atomic<std::uint8_t> alarm_volume{ CONFIG_ISOVT_AUDIO_ALARM_VOLUME };
std::atomic<bool> audio_muted{ false };
std::atomic<std::uint8_t> active_alarm_signal{ 4 };
std::atomic<std::uint8_t> pending_alarm_signal{ 4 };
double phase = 0.0;

bool take_pending_command(SignalCommand &command)
{
    return xQueueReceive(signal_queue, &command, 0) == pdTRUE;
}

bool write_audio(std::uint32_t duration_ms,
                 std::uint16_t start_frequency_hz,
                 std::uint16_t end_frequency_hz,
                 std::uint8_t volume_percent,
                 SignalCommand &pending)
{
    std::array<std::int16_t, FRAMES_PER_BLOCK * 2> samples = {};
    std::uint32_t remaining_frames = (SAMPLE_RATE * duration_ms) / 1000U;
    const std::uint32_t total_frames = remaining_frames;
    const std::uint8_t effective_volume = audio_muted.load() ? 0U : std::min<std::uint8_t>(volume_percent, 100U);
    const std::int32_t amplitude = (28000 * effective_volume) / 100;
    std::uint32_t rendered_frames = 0;

    while (remaining_frames > 0U)
    {
        if (take_pending_command(pending))
        {
            return false;
        }

        const std::size_t frames = std::min<std::uint32_t>(remaining_frames, FRAMES_PER_BLOCK);
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const double progress = (total_frames > 1U) ?
                                        static_cast<double>(rendered_frames) / (total_frames - 1U) :
                                        0.0;
            const double frequency_hz = start_frequency_hz +
                                        (static_cast<double>(end_frequency_hz) - start_frequency_hz) * progress;
            const double phase_step = TWO_PI * std::clamp(frequency_hz, 100.0, 10000.0) / SAMPLE_RATE;
            const std::int16_t sample = (start_frequency_hz == 0U) ?
                                            0 :
                                            static_cast<std::int16_t>(std::sin(phase) * amplitude);
            samples[frame * 2] = sample;
            samples[frame * 2 + 1] = sample;
            phase += phase_step;
            if (phase >= TWO_PI)
            {
                phase -= TWO_PI;
            }
            ++rendered_frames;
        }

        std::size_t written = 0;
        const esp_err_t result = i2s_channel_write(tx_channel,
                                                   samples.data(),
                                                   frames * 2U * sizeof(std::int16_t),
                                                   &written,
                                                   pdMS_TO_TICKS(100));
        if (result != ESP_OK)
        {
            ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(result));
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        remaining_frames -= static_cast<std::uint32_t>(frames);
    }
    return true;
}

void silence()
{
    std::array<std::int16_t, FRAMES_PER_BLOCK * 2> samples = {};
    std::size_t written = 0;
    i2s_channel_write(tx_channel,
                      samples.data(),
                      samples.size() * sizeof(std::int16_t),
                      &written,
                      pdMS_TO_TICKS(100));
}

bool play_sweep(std::uint16_t start_frequency_hz,
                std::uint16_t end_frequency_hz,
                std::uint8_t volume_percent,
                std::uint32_t duration_ms,
                SignalCommand &pending)
{
    return write_audio(duration_ms,
                       start_frequency_hz,
                       end_frequency_hz,
                       volume_percent,
                       pending);
}

bool play_interval(std::uint16_t frequency_hz,
                   std::uint8_t volume_percent,
                   std::uint32_t on_ms,
                   std::uint32_t off_ms,
                   SignalCommand &pending)
{
    if (!write_audio(on_ms, frequency_hz, frequency_hz, volume_percent, pending))
    {
        silence();
        return false;
    }
    if ((off_ms > 0U) && !write_audio(off_ms, 0, 0, 0, pending))
    {
        return false;
    }
    silence();
    return true;
}

bool play_alarm_once(std::uint8_t signal, SignalCommand &pending)
{
    const std::uint8_t volume = alarm_volume.load();
    ESP_LOGI(TAG, "Playing alarm %u at %u%% (muted=%u)",
             static_cast<unsigned>(signal),
             static_cast<unsigned>(volume),
             audio_muted.load() ? 1U : 0U);
    switch (signal)
    {
        case 0:
            if (!play_sweep(1100, 1500, volume, 220, pending))
            {
                silence();
                return false;
            }
            if (!play_sweep(2020, 2020, volume, 980, pending))
            {
                silence();
                return false;
            }
            silence();
            return true;
        case 1:
            for (int pulse = 0; pulse < 2; ++pulse)
            {
                if (!play_interval(920, volume, 220, 55, pending) ||
                    !play_interval(1380, volume, 220, (pulse == 0) ? 110 : 0, pending))
                {
                    return false;
                }
            }
            return true;
        case 3:
            if (!play_interval(1800, volume, 40, 30, pending))
            {
                return false;
            }
            for (int pulse = 0; pulse < 4; ++pulse)
            {
                if (!play_interval(1220,
                                   volume,
                                   70,
                                   (pulse < 3) ? 30 : 0,
                                   pending))
                {
                    return false;
                }
            }
            return true;
        default:
            return true;
    }
}

void audio_task(void *)
{
    SignalCommand command = {};
    bool has_command = false;
    silence();

    while (true)
    {
        if (!has_command)
        {
            const std::uint8_t alarm = pending_alarm_signal.exchange(4U);
            if (alarm != 4U)
            {
                ESP_LOGI(TAG, "Alarm mask signal: %u", static_cast<unsigned>(alarm));
                if (!play_alarm_once(alarm, command))
                {
                    has_command = true;
                }
                continue;
            }
            has_command = (xQueueReceive(signal_queue, &command, 0) == pdTRUE);
            if (!has_command)
            {
                // Keep the I2S DMA supplied with zero samples. Some MAX98357A
                // boards otherwise repeat stale data after the last tone.
                silence();
                continue;
            }
        }

        if (!has_command)
        {
            continue;
        }

        has_command = false;
        if ((command.activations == 0U) || (command.frequency_hz == 0U) || (command.duration_ms == 0U))
        {
            silence();
            continue;
        }

        ESP_LOGI(TAG, "Audio signal: %u x %u Hz, %u/%u ms",
                 static_cast<unsigned>(command.activations),
                 static_cast<unsigned>(command.frequency_hz),
                 static_cast<unsigned>(command.duration_ms),
                 static_cast<unsigned>(command.off_time_ms));
        for (std::uint16_t activation = 0; activation < command.activations; ++activation)
        {
            SignalCommand replacement = {};
            const std::uint32_t off_time = (activation + 1U < command.activations) ? command.off_time_ms : 0U;
            std::uint8_t volume = control_volume.load();
            if (command.apply_machine_volume)
            {
                volume = static_cast<std::uint8_t>((static_cast<std::uint16_t>(volume) *
                                                    requested_control_volume.load()) /
                                                   100U);
            }
            if (!play_interval(command.frequency_hz,
                               volume,
                               command.duration_ms,
                               off_time,
                               replacement))
            {
                command = replacement;
                has_command = true;
                break;
            }
        }
    }
}
}

namespace vt_audio
{
esp_err_t start()
{
#if CONFIG_ISOVT_ENABLE_AUDIO
    if (audio_task_handle != nullptr)
    {
        return ESP_OK;
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = FRAMES_PER_BLOCK;
    esp_err_t result = i2s_new_channel(&channel_config, &tx_channel, nullptr);
    if (result != ESP_OK)
    {
        return result;
    }

    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = static_cast<gpio_num_t>(CONFIG_ISOVT_AUDIO_I2S_BCLK_GPIO),
            .ws = static_cast<gpio_num_t>(CONFIG_ISOVT_AUDIO_I2S_WS_GPIO),
            .dout = static_cast<gpio_num_t>(CONFIG_ISOVT_AUDIO_I2S_DOUT_GPIO),
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    result = i2s_channel_init_std_mode(tx_channel, &standard_config);
    if (result == ESP_OK)
    {
        result = i2s_channel_enable(tx_channel);
    }
    if (result != ESP_OK)
    {
        i2s_del_channel(tx_channel);
        tx_channel = nullptr;
        return result;
    }

    signal_queue = xQueueCreate(1, sizeof(SignalCommand));
    if (signal_queue == nullptr)
    {
        i2s_channel_disable(tx_channel);
        i2s_del_channel(tx_channel);
        tx_channel = nullptr;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(audio_task, "vt_audio", 4096, nullptr, 4, &audio_task_handle, 0) != pdPASS)
    {
        vQueueDelete(signal_queue);
        signal_queue = nullptr;
        i2s_channel_disable(tx_channel);
        i2s_del_channel(tx_channel);
        tx_channel = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "MAX98357A I2S ready: BCLK=%d WS=%d DIN=%d",
             CONFIG_ISOVT_AUDIO_I2S_BCLK_GPIO,
             CONFIG_ISOVT_AUDIO_I2S_WS_GPIO,
             CONFIG_ISOVT_AUDIO_I2S_DOUT_GPIO);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool is_enabled()
{
#if CONFIG_ISOVT_ENABLE_AUDIO
    return audio_task_handle != nullptr;
#else
    return false;
#endif
}

void play_control_signal(std::uint8_t activations,
                         std::uint16_t frequency_hz,
                         std::uint16_t duration_ms,
                         std::uint16_t off_time_ms)
{
    if (signal_queue == nullptr)
    {
        return;
    }
    const SignalCommand command = { activations, frequency_hz, duration_ms, off_time_ms, true };
    xQueueOverwrite(signal_queue, &command);
}

void set_control_volume(std::uint8_t volume_percent)
{
    control_volume.store(std::min<std::uint8_t>(volume_percent, 100U));
}

void set_requested_control_volume(std::uint8_t volume_percent)
{
    requested_control_volume.store(std::min<std::uint8_t>(volume_percent, 100U));
    ESP_LOGI(TAG, "Machine requested audio volume: %u%% (terminal limit: %u%%)",
             static_cast<unsigned>(requested_control_volume.load()),
             static_cast<unsigned>(control_volume.load()));
}

void set_alarm_volume(std::uint8_t volume_percent)
{
    alarm_volume.store(std::min<std::uint8_t>(volume_percent, 100U));
}

void set_muted(bool muted)
{
    audio_muted.store(muted);
    if (muted && (signal_queue != nullptr))
    {
        const SignalCommand stop = {};
        xQueueOverwrite(signal_queue, &stop);
    }
}

std::uint8_t get_control_volume()
{
    return control_volume.load();
}

std::uint8_t get_alarm_volume()
{
    return alarm_volume.load();
}

bool is_muted()
{
    return audio_muted.load();
}

void play_test_control()
{
    if (signal_queue == nullptr)
    {
        return;
    }
    const SignalCommand command = { 2, 1200, 160, 100, false };
    xQueueOverwrite(signal_queue, &command);
}

void play_test_alarm()
{
    pending_alarm_signal.store(1U);
}

void set_alarm_signal(std::uint8_t acoustic_signal)
{
    const std::uint8_t normalized = ((acoustic_signal == 0U) || (acoustic_signal == 1U) || (acoustic_signal == 3U)) ? acoustic_signal : 4U;
    if (active_alarm_signal.exchange(normalized) == normalized)
    {
        return;
    }
    pending_alarm_signal.store(normalized);
}
}
