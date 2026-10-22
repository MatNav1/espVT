#include "gnss_um982.hpp"

#include "lvgl_terminal_view.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"

namespace
{
constexpr char TAG[] = "um982";
constexpr std::size_t NMEA_LINE_CAPACITY = 160;
constexpr std::size_t UART_BUFFER_SIZE = 4096;
constexpr EventBits_t WIFI_HAS_IP = BIT0;

#if CONFIG_ISOVT_GNSS_SOURCE_UM982
constexpr int GNSS_UART_PORT = CONFIG_ISOVT_UM982_UART_PORT;
constexpr int GNSS_UART_RX_GPIO = CONFIG_ISOVT_UM982_UART_RX_GPIO;
constexpr int GNSS_UART_TX_GPIO = CONFIG_ISOVT_UM982_UART_TX_GPIO;
constexpr int GNSS_UART_BAUD_RATE = CONFIG_ISOVT_UM982_UART_BAUD_RATE;
constexpr char GNSS_RECEIVER_LABEL[] = "UM982";
#elif CONFIG_ISOVT_GNSS_SOURCE_RS232
constexpr int GNSS_UART_PORT = CONFIG_ISOVT_RS232_UART_PORT;
constexpr int GNSS_UART_RX_GPIO = CONFIG_ISOVT_RS232_UART_RX_GPIO;
constexpr int GNSS_UART_TX_GPIO = CONFIG_ISOVT_RS232_UART_TX_GPIO;
constexpr int GNSS_UART_BAUD_RATE = CONFIG_ISOVT_RS232_UART_BAUD_RATE;
constexpr char GNSS_RECEIVER_LABEL[] = "RS232 GPS";
#endif

struct SharedState
{
    gnss_um982::Snapshot snapshot = {};
    std::array<char, NMEA_LINE_CAPACITY> last_gga = {};
    bool has_gga = false;
    std::int64_t last_view_update_ms = 0;
};

SharedState state = {};
SemaphoreHandle_t state_mutex = nullptr;
std::uint32_t uart_received_bytes = 0;
std::uint32_t valid_gga_sentences = 0;
std::uint32_t valid_vtg_sentences = 0;
std::uint32_t valid_rmc_sentences = 0;
std::uint32_t invalid_checksum_sentences = 0;
#if CONFIG_ISOVT_GNSS_SOURCE_UM982 && CONFIG_ISOVT_ENABLE_NTRIP
EventGroupHandle_t wifi_events = nullptr;
esp_netif_t *wifi_station = nullptr;
bool wifi_started = false;
#endif

std::int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

bool take_state_lock()
{
    return (state_mutex != nullptr) && (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
}

#if CONFIG_ISOVT_GNSS_SOURCE_UM982 || CONFIG_ISOVT_GNSS_SOURCE_RS232
bool parse_double(const char *text, double &value)
{
    if ((text == nullptr) || (*text == '\0'))
    {
        return false;
    }

    char *end = nullptr;
    errno = 0;
    value = std::strtod(text, &end);
    return (end != text) && (*end == '\0') && (errno == 0) && std::isfinite(value);
}

bool parse_coordinate(const char *value, const char *hemisphere, double &degrees)
{
    if ((hemisphere == nullptr) || (std::strlen(hemisphere) != 1))
    {
        return false;
    }

    double packed_degrees = 0.0;
    if (!parse_double(value, packed_degrees))
    {
        return false;
    }

    const int whole_degrees = static_cast<int>(packed_degrees / 100.0);
    const double minutes = packed_degrees - static_cast<double>(whole_degrees * 100);
    if ((minutes < 0.0) || (minutes >= 60.0))
    {
        return false;
    }

    degrees = static_cast<double>(whole_degrees) + minutes / 60.0;
    if ((*hemisphere == 'S') || (*hemisphere == 'W'))
    {
        degrees = -degrees;
    }
    return (*hemisphere == 'N') || (*hemisphere == 'S') || (*hemisphere == 'E') || (*hemisphere == 'W');
}

bool nmea_checksum_is_valid(const char *sentence)
{
    if ((sentence == nullptr) || (sentence[0] != '$'))
    {
        return false;
    }

    const char *asterisk = std::strchr(sentence, '*');
    if ((asterisk == nullptr) || (std::strlen(asterisk) != 3))
    {
        return false;
    }

    std::uint8_t calculated = 0;
    for (const char *cursor = sentence + 1; cursor < asterisk; ++cursor)
    {
        calculated ^= static_cast<std::uint8_t>(*cursor);
    }

    char *end = nullptr;
    const long expected = std::strtol(asterisk + 1, &end, 16);
    return (end == (asterisk + 3)) && (expected >= 0) && (expected <= 0xFF) && (calculated == expected);
}

gnss_um982::FixQuality fix_quality_from_gga(const char *text)
{
    if ((text == nullptr) || (*text == '\0'))
    {
        return gnss_um982::FixQuality::NoFix;
    }

    switch (std::strtol(text, nullptr, 10))
    {
        case 1:
            return gnss_um982::FixQuality::Autonomous;
        case 2:
            return gnss_um982::FixQuality::Differential;
        case 4:
            return gnss_um982::FixQuality::RtkFixed;
        case 5:
            return gnss_um982::FixQuality::RtkFloat;
        default:
            return gnss_um982::FixQuality::NoFix;
    }
}

void update_view(bool force = false)
{
    gnss_um982::Snapshot snapshot;
    bool should_update = false;
    if (take_state_lock())
    {
        const std::int64_t timestamp = now_ms();
        should_update = force || ((timestamp - state.last_view_update_ms) >= 2000);
        if (should_update)
        {
            state.last_view_update_ms = timestamp;
        }
        snapshot = state.snapshot;
        xSemaphoreGive(state_mutex);
    }
    if (!should_update)
    {
        return;
    }

    char message[64] = {};
    if (!snapshot.receiver_configured)
    {
        std::snprintf(message, sizeof(message), "GNSS UART PINS REQUIRED");
    }
    else if (!snapshot.position_valid)
    {
        std::snprintf(message, sizeof(message), "GNSS NO FIX%s", snapshot.ntrip_enabled ? " NTRIP WAIT" : "");
    }
    else if (snapshot.ntrip_connected)
    {
        std::snprintf(message, sizeof(message), "GNSS %s %u SAT NTRIP ON", gnss_um982::fix_quality_text(snapshot.fix_quality), snapshot.satellites);
    }
    else
    {
        std::snprintf(message, sizeof(message), "GNSS %s %u SAT%s", gnss_um982::fix_quality_text(snapshot.fix_quality), snapshot.satellites, snapshot.ntrip_enabled ? " NTRIP WAIT" : "");
    }
    vt_view::set_gnss_status(message);
}

void parse_nmea_sentence(char *sentence)
{
    if (!nmea_checksum_is_valid(sentence))
    {
        ++invalid_checksum_sentences;
        return;
    }

    std::array<char, NMEA_LINE_CAPACITY> original_sentence = {};
    std::snprintf(original_sentence.data(), original_sentence.size(), "%s", sentence);
    char *asterisk = std::strchr(sentence, '*');
    *asterisk = '\0';
    std::array<char *, 20> fields = {};
    std::size_t field_count = 0;
    char *cursor = sentence;
    while ((cursor != nullptr) && (field_count < fields.size()))
    {
        fields[field_count++] = cursor;
        cursor = std::strchr(cursor, ',');
        if (cursor != nullptr)
        {
            *cursor = '\0';
            ++cursor;
        }
    }
    if ((field_count == 0) || (std::strlen(fields[0]) < 4))
    {
        return;
    }

    const char *type = fields[0] + std::strlen(fields[0]) - 3;
    bool updated = false;
    if ((std::strcmp(type, "GGA") == 0) && (field_count >= 9))
    {
        ++valid_gga_sentences;
        double latitude = 0.0;
        double longitude = 0.0;
        const bool coordinates_valid = parse_coordinate(fields[2], fields[3], latitude) && parse_coordinate(fields[4], fields[5], longitude);
        double hdop = 0.0;
        parse_double(fields[8], hdop);
        const long satellite_count = std::strtol(fields[7], nullptr, 10);

        if (take_state_lock())
        {
            state.snapshot.fix_quality = fix_quality_from_gga(fields[6]);
            state.snapshot.satellites = static_cast<std::uint8_t>(std::clamp(satellite_count, 0L, 99L));
            state.snapshot.hdop = static_cast<float>(hdop);
            state.snapshot.position_valid = coordinates_valid && (state.snapshot.fix_quality != gnss_um982::FixQuality::NoFix);
            if (coordinates_valid)
            {
                state.snapshot.latitude_degrees = latitude;
                state.snapshot.longitude_degrees = longitude;
                state.snapshot.last_position_ms = now_ms();
                std::snprintf(state.last_gga.data(), state.last_gga.size(), "%s", original_sentence.data());
                state.has_gga = true;
            }
            xSemaphoreGive(state_mutex);
            updated = true;
        }
    }
    else if ((std::strcmp(type, "RMC") == 0) && (field_count >= 9) && (std::strcmp(fields[2], "A") == 0))
    {
        ++valid_rmc_sentences;
        double latitude = 0.0;
        double longitude = 0.0;
        double speed_knots = 0.0;
        double course = 0.0;
        const bool coordinates_valid = parse_coordinate(fields[3], fields[4], latitude) && parse_coordinate(fields[5], fields[6], longitude);
        parse_double(fields[7], speed_knots);
        parse_double(fields[8], course);

        if (take_state_lock())
        {
            state.snapshot.position_valid = coordinates_valid;
            state.snapshot.latitude_degrees = latitude;
            state.snapshot.longitude_degrees = longitude;
            state.snapshot.speed_mps = static_cast<float>(speed_knots * 0.514444);
            state.snapshot.course_degrees = static_cast<float>(course);
            state.snapshot.last_position_ms = now_ms();
            state.snapshot.last_velocity_ms = state.snapshot.last_position_ms;
            xSemaphoreGive(state_mutex);
            updated = true;
        }
    }
    else if ((std::strcmp(type, "VTG") == 0) && (field_count >= 8))
    {
        ++valid_vtg_sentences;
        double course = 0.0;
        double speed_kmh = 0.0;
        double speed_knots = 0.0;
        const bool course_valid = parse_double(fields[1], course);
        bool speed_valid = parse_double(fields[7], speed_kmh) && (speed_kmh >= 0.0);
        if (!speed_valid && parse_double(fields[5], speed_knots) && (speed_knots >= 0.0))
        {
            speed_kmh = speed_knots * 1.852;
            speed_valid = true;
        }

        // NMEA 2.3+ can append a mode field. "N" means that navigation data
        // is invalid; older VTG sentences legitimately omit this field.
        const bool mode_valid = (field_count < 10) || (fields[9][0] == '\0') || (fields[9][0] != 'N');
        if (speed_valid && mode_valid && take_state_lock())
        {
            state.snapshot.speed_mps = static_cast<float>(speed_kmh / 3.6);
            if (course_valid)
            {
                state.snapshot.course_degrees = static_cast<float>(course);
            }
            state.snapshot.last_velocity_ms = now_ms();
            xSemaphoreGive(state_mutex);
            updated = true;
        }
    }

    if (updated)
    {
        update_view();
    }
}

#if CONFIG_ISOVT_GNSS_SOURCE_RS232
int detect_rs232_baud_rate()
{
    constexpr std::array<int, 6> COMMON_BAUD_RATES = {
        GNSS_UART_BAUD_RATE, 4800, 9600, 19200, 38400, 115200
    };
    const uart_port_t uart = static_cast<uart_port_t>(GNSS_UART_PORT);

    for (std::size_t candidate_index = 0; candidate_index < COMMON_BAUD_RATES.size(); ++candidate_index)
    {
        const int baud_rate = COMMON_BAUD_RATES[candidate_index];
        bool already_tested = false;
        for (std::size_t earlier = 0; earlier < candidate_index; ++earlier)
        {
            already_tested = already_tested || (COMMON_BAUD_RATES[earlier] == baud_rate);
        }
        if (already_tested)
        {
            continue;
        }

        uart_set_baudrate(uart, baud_rate);
        uart_flush_input(uart);

        std::array<std::uint8_t, 128> received = {};
        std::array<char, NMEA_LINE_CAPACITY> line = {};
        std::size_t line_length = 0;
        std::uint32_t byte_count = 0;
        std::uint32_t valid_sentences = 0;
        const std::int64_t deadline_ms = now_ms() + 1400;

        while (now_ms() < deadline_ms)
        {
            const int read = uart_read_bytes(uart, received.data(), received.size(), pdMS_TO_TICKS(100));
            if (read > 0)
            {
                byte_count += static_cast<std::uint32_t>(read);
            }
            for (int index = 0; index < read; ++index)
            {
                const char character = static_cast<char>(received[static_cast<std::size_t>(index)]);
                if (character == '$')
                {
                    line_length = 0;
                    line[line_length++] = character;
                }
                else if (character == '\n')
                {
                    if (line_length > 0)
                    {
                        line[line_length] = '\0';
                        valid_sentences += nmea_checksum_is_valid(line.data()) ? 1U : 0U;
                        line_length = 0;
                    }
                }
                else if ((character != '\r') && (line_length > 0) && (line_length < (line.size() - 1)))
                {
                    line[line_length++] = character;
                }
            }
        }

        ESP_LOGI(TAG, "RS232 probe %d bit/s: bytes=%lu valid NMEA=%lu",
                 baud_rate,
                 static_cast<unsigned long>(byte_count),
                 static_cast<unsigned long>(valid_sentences));
        if (valid_sentences > 0)
        {
            ESP_LOGI(TAG, "RS232 detected %d bit/s", baud_rate);
            return baud_rate;
        }
    }

    uart_set_baudrate(uart, GNSS_UART_BAUD_RATE);
    ESP_LOGW(TAG, "RS232 auto-detection found no valid NMEA; using configured %d bit/s", GNSS_UART_BAUD_RATE);
    return GNSS_UART_BAUD_RATE;
}
#endif

void gnss_receive_task(void *)
{
#if CONFIG_ISOVT_GNSS_SOURCE_RS232
    detect_rs232_baud_rate();
#endif
    std::array<std::uint8_t, 256> received = {};
    std::array<char, NMEA_LINE_CAPACITY> line = {};
    std::size_t line_length = 0;
    std::int64_t last_diagnostic_ms = now_ms();

    while (true)
    {
        const int read = uart_read_bytes(static_cast<uart_port_t>(GNSS_UART_PORT), received.data(), received.size(), pdMS_TO_TICKS(200));
        if (read > 0)
        {
            uart_received_bytes += static_cast<std::uint32_t>(read);
        }
        for (int index = 0; index < read; ++index)
        {
            const char character = static_cast<char>(received[static_cast<std::size_t>(index)]);
            if (character == '$')
            {
                line_length = 0;
                line[line_length++] = character;
            }
            else if (character == '\n')
            {
                if (line_length > 0)
                {
                    line[line_length] = '\0';
                    parse_nmea_sentence(line.data());
                    line_length = 0;
                }
            }
            else if ((character != '\r') && (line_length > 0) && (line_length < (line.size() - 1)))
            {
                line[line_length++] = character;
            }
        }

        const std::int64_t timestamp = now_ms();
        if ((timestamp - last_diagnostic_ms) >= 5000)
        {
            gnss_um982::Snapshot snapshot;
            if (take_state_lock())
            {
                snapshot = state.snapshot;
                xSemaphoreGive(state_mutex);
            }
            ESP_LOGI(TAG,
                     "RS232 RX bytes=%lu GGA=%lu VTG=%lu RMC=%lu bad=%lu fix=%s speed=%.2f km/h",
                     static_cast<unsigned long>(uart_received_bytes),
                     static_cast<unsigned long>(valid_gga_sentences),
                     static_cast<unsigned long>(valid_vtg_sentences),
                     static_cast<unsigned long>(valid_rmc_sentences),
                     static_cast<unsigned long>(invalid_checksum_sentences),
                     gnss_um982::fix_quality_text(snapshot.fix_quality),
                     snapshot.speed_mps * 3.6F);
            last_diagnostic_ms = timestamp;
        }
    }
}
#endif

#if CONFIG_ISOVT_GNSS_SOURCE_UM982 && CONFIG_ISOVT_ENABLE_NTRIP
void set_ntrip_connected(bool connected)
{
    if (take_state_lock())
    {
        state.snapshot.ntrip_connected = connected;
        xSemaphoreGive(state_mutex);
    }
    update_view(true);
}

void wifi_event_handler(void *, esp_event_base_t event_base, std::int32_t event_id, void *)
{
    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_STA_START)
        {
            esp_wifi_connect();
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            xEventGroupClearBits(wifi_events, WIFI_HAS_IP);
            set_ntrip_connected(false);
            esp_wifi_connect();
        }
    }
    else if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP))
    {
        xEventGroupSetBits(wifi_events, WIFI_HAS_IP);
    }
}

esp_err_t start_wifi_station()
{
    if (wifi_started)
    {
        return ESP_OK;
    }
    if ((std::strlen(CONFIG_ISOVT_WIFI_SSID) == 0) || (std::strlen(CONFIG_ISOVT_NTRIP_HOST) == 0) ||
        (std::strlen(CONFIG_ISOVT_NTRIP_MOUNTPOINT) == 0))
    {
        ESP_LOGW(TAG, "NTRIP configuration is incomplete");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = esp_netif_init();
    if ((result != ESP_OK) && (result != ESP_ERR_INVALID_STATE))
    {
        return result;
    }
    result = esp_event_loop_create_default();
    if ((result != ESP_OK) && (result != ESP_ERR_INVALID_STATE))
    {
        return result;
    }
    wifi_station = esp_netif_create_default_wifi_sta();
    if (wifi_station == nullptr)
    {
        return ESP_FAIL;
    }

    wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&initialization), TAG, "Wi-Fi initialization failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr), TAG, "Wi-Fi event registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr), TAG, "IP event registration failed");

    wifi_config_t configuration = {};
    std::snprintf(reinterpret_cast<char *>(configuration.sta.ssid), sizeof(configuration.sta.ssid), "%s", CONFIG_ISOVT_WIFI_SSID);
    std::snprintf(reinterpret_cast<char *>(configuration.sta.password), sizeof(configuration.sta.password), "%s", CONFIG_ISOVT_WIFI_PASSWORD);
    configuration.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    configuration.sta.pmf_cfg.capable = true;
    configuration.sta.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi station mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &configuration), TAG, "Wi-Fi configuration failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");
    wifi_started = true;
    ESP_LOGI(TAG, "Wi-Fi station started for NTRIP");
    return ESP_OK;
}

bool send_all(int socket, const std::uint8_t *data, std::size_t length)
{
    while (length > 0)
    {
        const int written = send(socket, data, length, 0);
        if (written <= 0)
        {
            return false;
        }
        data += written;
        length -= static_cast<std::size_t>(written);
    }
    return true;
}

int connect_ntrip_socket()
{
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    const std::string port = std::to_string(CONFIG_ISOVT_NTRIP_PORT);
    if (getaddrinfo(CONFIG_ISOVT_NTRIP_HOST, port.c_str(), &hints, &addresses) != 0)
    {
        return -1;
    }

    int socket = -1;
    for (addrinfo *address = addresses; address != nullptr; address = address->ai_next)
    {
        socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if ((socket >= 0) && (connect(socket, address->ai_addr, address->ai_addrlen) == 0))
        {
            break;
        }
        if (socket >= 0)
        {
            close(socket);
            socket = -1;
        }
    }
    freeaddrinfo(addresses);
    return socket;
}

bool send_ntrip_request(int socket)
{
    std::string authorization;
    if (std::strlen(CONFIG_ISOVT_NTRIP_USERNAME) > 0)
    {
        const std::string credentials = std::string(CONFIG_ISOVT_NTRIP_USERNAME) + ":" + CONFIG_ISOVT_NTRIP_PASSWORD;
        std::array<unsigned char, 192> encoded = {};
        std::size_t encoded_length = 0;
        if (mbedtls_base64_encode(encoded.data(), encoded.size(), &encoded_length,
                                  reinterpret_cast<const unsigned char *>(credentials.data()), credentials.size()) != 0)
        {
            return false;
        }
        authorization.assign(reinterpret_cast<const char *>(encoded.data()), encoded_length);
    }

    std::string request = "GET /" + std::string(CONFIG_ISOVT_NTRIP_MOUNTPOINT) + " HTTP/1.0\r\n";
    request += "User-Agent: NTRIP ESP-ISOBUS-VT/1.0\r\n";
    request += "Host: " + std::string(CONFIG_ISOVT_NTRIP_HOST) + "\r\n";
    request += "Ntrip-Version: Ntrip/2.0\r\n";
    if (!authorization.empty())
    {
        request += "Authorization: Basic " + authorization + "\r\n";
    }
    request += "Connection: close\r\n\r\n";
    return send_all(socket, reinterpret_cast<const std::uint8_t *>(request.data()), request.size());
}

std::size_t find_header_end(const std::uint8_t *data, std::size_t length)
{
    if ((length >= 12) && (std::memcmp(data, "ICY 200 OK\r\n", 12) == 0))
    {
        return 12;
    }
    for (std::size_t index = 3; index < length; ++index)
    {
        if ((data[index - 3] == '\r') && (data[index - 2] == '\n') && (data[index - 1] == '\r') && (data[index] == '\n'))
        {
            return index + 1;
        }
    }
    return 0;
}

void forward_rtcm(const std::uint8_t *data, std::size_t length)
{
    if (length == 0)
    {
        return;
    }
    const int written = uart_write_bytes(static_cast<uart_port_t>(GNSS_UART_PORT), data, length);
    if (written > 0 && take_state_lock())
    {
        state.snapshot.last_rtcm_ms = now_ms();
        state.snapshot.rtcm_bytes += static_cast<std::uint32_t>(written);
        xSemaphoreGive(state_mutex);
    }
}

bool receive_ntrip_header(int socket)
{
    std::array<std::uint8_t, 1024> received = {};
    std::size_t used = 0;
    while (used < received.size())
    {
        const int read = recv(socket, received.data() + used, received.size() - used, 0);
        if (read <= 0)
        {
            return false;
        }
        used += static_cast<std::size_t>(read);
        const std::size_t header_end = find_header_end(received.data(), used);
        if (header_end == 0)
        {
            continue;
        }

        const std::string header(reinterpret_cast<const char *>(received.data()), header_end);
        if (header.find("200") == std::string::npos)
        {
            ESP_LOGW(TAG, "NTRIP rejected request: %s", header.c_str());
            return false;
        }
        forward_rtcm(received.data() + header_end, used - header_end);
        return true;
    }
    return false;
}

bool send_latest_gga(int socket)
{
    std::array<char, NMEA_LINE_CAPACITY + 2> gga = {};
    if (take_state_lock())
    {
        if (state.has_gga)
        {
            std::snprintf(gga.data(), gga.size(), "%s\r\n", state.last_gga.data());
        }
        xSemaphoreGive(state_mutex);
    }
    return (gga[0] == '\0') || send_all(socket, reinterpret_cast<const std::uint8_t *>(gga.data()), std::strlen(gga.data()));
}

void ntrip_receive_task(void *)
{
    while (true)
    {
        xEventGroupWaitBits(wifi_events, WIFI_HAS_IP, pdFALSE, pdTRUE, portMAX_DELAY);
        update_view(true);

        const int socket = connect_ntrip_socket();
        if ((socket < 0) || !send_ntrip_request(socket) || !receive_ntrip_header(socket))
        {
            if (socket >= 0)
            {
                close(socket);
            }
            set_ntrip_connected(false);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        timeval timeout = {};
        timeout.tv_sec = 1;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        set_ntrip_connected(true);
        std::int64_t next_gga_ms = now_ms();
        std::array<std::uint8_t, 1024> received = {};
        bool active = true;
        while (active && ((xEventGroupGetBits(wifi_events) & WIFI_HAS_IP) != 0))
        {
            const std::int64_t timestamp = now_ms();
            if (timestamp >= next_gga_ms)
            {
                active = send_latest_gga(socket);
                next_gga_ms = timestamp + 5000;
            }

            const int read = recv(socket, received.data(), received.size(), 0);
            if (read > 0)
            {
                forward_rtcm(received.data(), static_cast<std::size_t>(read));
            }
            else if ((read == 0) || ((errno != EAGAIN) && (errno != EWOULDBLOCK)))
            {
                active = false;
            }
        }
        close(socket);
        set_ntrip_connected(false);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif
}

namespace gnss_um982
{
bool is_enabled()
{
#if CONFIG_ISOVT_GNSS_SOURCE_UM982 || CONFIG_ISOVT_GNSS_SOURCE_RS232
    return true;
#else
    return false;
#endif
}

esp_err_t start()
{
#if !CONFIG_ISOVT_GNSS_SOURCE_UM982 && !CONFIG_ISOVT_GNSS_SOURCE_RS232
    vt_view::set_gnss_status("GNSS OPTIONAL MODULE OFF");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (GNSS_UART_RX_GPIO < 0)
    {
        vt_view::set_gnss_status("GNSS UART PINS REQUIRED");
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_ISOVT_GNSS_SOURCE_UM982 && CONFIG_ISOVT_ENABLE_NTRIP
    if (GNSS_UART_TX_GPIO < 0)
    {
        vt_view::set_gnss_status("GNSS NTRIP TX REQUIRED");
        return ESP_ERR_INVALID_ARG;
    }
#endif

    state_mutex = xSemaphoreCreateMutex();
    if (state_mutex == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }
    if (take_state_lock())
    {
        state.snapshot.receiver_configured = true;
#if CONFIG_ISOVT_GNSS_SOURCE_UM982 && CONFIG_ISOVT_ENABLE_NTRIP
        state.snapshot.ntrip_enabled = true;
#endif
        xSemaphoreGive(state_mutex);
    }

    uart_config_t configuration = {};
    configuration.baud_rate = GNSS_UART_BAUD_RATE;
    configuration.data_bits = UART_DATA_8_BITS;
    configuration.parity = UART_PARITY_DISABLE;
    configuration.stop_bits = UART_STOP_BITS_1;
    configuration.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    configuration.source_clk = UART_SCLK_DEFAULT;
    const uart_port_t uart = static_cast<uart_port_t>(GNSS_UART_PORT);
    ESP_RETURN_ON_ERROR(uart_param_config(uart, &configuration), TAG, "GNSS UART configuration failed");
    const int tx_gpio = (GNSS_UART_TX_GPIO >= 0) ? GNSS_UART_TX_GPIO : UART_PIN_NO_CHANGE;
    ESP_RETURN_ON_ERROR(uart_set_pin(uart, tx_gpio, GNSS_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "GNSS UART pins failed");
    ESP_RETURN_ON_ERROR(uart_driver_install(uart, UART_BUFFER_SIZE, UART_BUFFER_SIZE, 0, nullptr, 0), TAG, "GNSS UART driver failed");
    if (xTaskCreate(gnss_receive_task, "um982_rx", 4096, nullptr, 5, nullptr) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_ISOVT_GNSS_SOURCE_UM982 && CONFIG_ISOVT_ENABLE_NTRIP
    wifi_events = xEventGroupCreate();
    if (wifi_events == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t wifi_result = start_wifi_station();
    if (wifi_result == ESP_OK)
    {
        if (xTaskCreate(ntrip_receive_task, "ntrip_rx", 6144, nullptr, 4, nullptr) != pdPASS)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    else
    {
        ESP_LOGW(TAG, "NTRIP is not running: %s", esp_err_to_name(wifi_result));
    }
#endif

    ESP_LOGI(TAG, "%s NMEA receiver started on UART%d RX=%d TX=%d", GNSS_RECEIVER_LABEL, GNSS_UART_PORT,
             GNSS_UART_RX_GPIO, GNSS_UART_TX_GPIO);
    update_view(true);
    return ESP_OK;
#endif
}

Snapshot get_snapshot()
{
    Snapshot snapshot = {};
    if (take_state_lock())
    {
        snapshot = state.snapshot;
        xSemaphoreGive(state_mutex);
    }
    return snapshot;
}

bool is_section_control_position_ready()
{
    const Snapshot snapshot = get_snapshot();
    const bool fresh_position = snapshot.position_valid && ((now_ms() - snapshot.last_position_ms) <= 1000);
    bool sufficient_fix = (snapshot.fix_quality == FixQuality::RtkFixed);
#if CONFIG_ISOVT_SECTION_CONTROL_ALLOW_RTK_FLOAT
    sufficient_fix = sufficient_fix || (snapshot.fix_quality == FixQuality::RtkFloat);
#endif
    if (!fresh_position || !sufficient_fix)
    {
        return false;
    }

    // An external RTK receiver reports its own fix quality over NMEA. When this
    // firmware supplies the corrections, their stream must be fresh as well.
    return !snapshot.ntrip_enabled || (snapshot.ntrip_connected && ((now_ms() - snapshot.last_rtcm_ms) <= 5000));
}

const char *fix_quality_text(FixQuality quality)
{
    switch (quality)
    {
        case FixQuality::NoFix:
            return "NO FIX";
        case FixQuality::Autonomous:
            return "GPS";
        case FixQuality::Differential:
            return "DGPS";
        case FixQuality::RtkFloat:
            return "RTK FLOAT";
        case FixQuality::RtkFixed:
            return "RTK FIX";
        default:
            return "OFF";
    }
}
}
