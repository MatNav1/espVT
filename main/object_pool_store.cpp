#include "object_pool_store.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <unistd.h>

namespace
{
constexpr char TAG[] = "pool_store";
constexpr char BASE_PATH[] = "/spiffs";
constexpr char CACHE_SCHEMA_MARKER[] = "/spiffs/.pool_cache_v2";
constexpr std::uint64_t CLIENT_FILE_KEY_MASK = 0x00000000FFFFFFFFULL;

std::string hex_label(const std::vector<std::uint8_t> &version_label)
{
    static constexpr char HEX[] = "0123456789abcdef";
    std::string result;
    result.reserve(version_label.size() * 2);
    for (const auto byte : version_label)
    {
        result.push_back(HEX[(byte >> 4) & 0x0F]);
        result.push_back(HEX[byte & 0x0F]);
    }
    return result.empty() ? "default" : result;
}

std::string path_for(std::uint64_t client_name, const std::vector<std::uint8_t> &version_label)
{
    char name[96] = {};
    std::snprintf(name, sizeof(name), "%s/%08llx_%s.iop",
                  BASE_PATH,
                  static_cast<unsigned long long>(client_name & CLIENT_FILE_KEY_MASK),
                  hex_label(version_label).c_str());
    return std::string(name);
}

bool has_iop_extension(const char *name)
{
    const std::size_t length = std::strlen(name);
    return (length > 4U) && (std::strcmp(name + length - 4U, ".iop") == 0);
}

bool decode_hex_byte(const char high, const char low, std::uint8_t &value)
{
    const auto to_nibble = [](const char character) -> int {
        if ((character >= '0') && (character <= '9'))
        {
            return character - '0';
        }
        if ((character >= 'a') && (character <= 'f'))
        {
            return character - 'a' + 10;
        }
        return -1;
    };

    const int high_nibble = to_nibble(high);
    const int low_nibble = to_nibble(low);
    if ((high_nibble < 0) || (low_nibble < 0))
    {
        return false;
    }

    value = static_cast<std::uint8_t>((high_nibble << 4) | low_nibble);
    return true;
}
}

namespace object_pool_store
{
bool init()
{
    esp_vfs_spiffs_conf_t config = {};
    config.base_path = BASE_PATH;
    config.partition_label = "storage";
    config.max_files = 16;
    config.format_if_mount_failed = true;

    const esp_err_t result = esp_vfs_spiffs_register(&config);
    if ((result != ESP_OK) && (result != ESP_ERR_INVALID_STATE))
    {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(result));
        return false;
    }

    // Pools saved by older firmware can still pass the client's unchanged
    // version label while no longer containing its newer dynamic string
    // objects. Invalidate that cache once so the ECU transfers a complete,
    // current pool and AUX designator labels can be resolved correctly.
    FILE *schema_marker = std::fopen(CACHE_SCHEMA_MARKER, "rb");
    if (schema_marker == nullptr)
    {
        const bool cleared = erase_all();
        schema_marker = std::fopen(CACHE_SCHEMA_MARKER, "wb");
        if (schema_marker == nullptr)
        {
            ESP_LOGE(TAG, "Could not create object-pool cache schema marker");
            return false;
        }
        constexpr char SCHEMA[] = "2";
        const bool marker_written = std::fwrite(SCHEMA, 1, sizeof(SCHEMA) - 1U, schema_marker) == (sizeof(SCHEMA) - 1U);
        std::fclose(schema_marker);
        if (!cleared || !marker_written)
        {
            ESP_LOGE(TAG, "Could not migrate object-pool cache");
            return false;
        }
        ESP_LOGI(TAG, "Old object-pool cache cleared; clients will transfer current pools");
    }
    else
    {
        std::fclose(schema_marker);
    }
    return true;
}

bool save(std::uint64_t client_name, const std::vector<std::uint8_t> &version_label, const std::vector<std::uint8_t> &data)
{
    const auto path = path_for(client_name, version_label);
    // This callback runs immediately after a potentially large object pool was
    // parsed, when internal RAM can be extremely tight. stdio's fopen() lazily
    // allocates a recursive FILE lock and can abort if that allocation fails.
    // The unbuffered POSIX path uses the already configured SPIFFS VFS without
    // allocating a new stdio stream or lock.
    const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (file < 0)
    {
        ESP_LOGE(TAG, "Could not open %s for writing: errno=%d", path.c_str(), errno);
        return false;
    }

    std::size_t written = 0;
    while (written < data.size())
    {
        const ssize_t result = ::write(file, data.data() + written, data.size() - written);
        if (result > 0)
        {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if ((result < 0) && (errno == EINTR))
        {
            continue;
        }
        break;
    }
    const int close_result = ::close(file);
    if ((written != data.size()) || (close_result != 0))
    {
        ESP_LOGE(TAG, "Short write to %s: %u/%u bytes", path.c_str(),
                 static_cast<unsigned>(written),
                 static_cast<unsigned>(data.size()));
        return false;
    }
    ESP_LOGI(TAG, "Stored %u object-pool bytes in %s",
             static_cast<unsigned>(written),
             path.c_str());
    return true;
}

std::vector<std::uint8_t> load(std::uint64_t client_name, const std::vector<std::uint8_t> &version_label)
{
    const auto path = path_for(client_name, version_label);
    FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
    {
        return {};
    }

    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    if (length < 0)
    {
        std::fclose(file);
        return {};
    }
    std::rewind(file);

    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    const auto read = data.empty() ? 0U : std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    if (read != data.size())
    {
        return {};
    }
    return data;
}

std::vector<std::array<std::uint8_t, 7>> list_versions(std::uint64_t client_name)
{
    std::vector<std::array<std::uint8_t, 7>> versions;
    char prefix[32] = {};
    std::snprintf(prefix, sizeof(prefix), "%08llx_",
                  static_cast<unsigned long long>(client_name & CLIENT_FILE_KEY_MASK));

    DIR *directory = opendir(BASE_PATH);
    if (directory == nullptr)
    {
        return versions;
    }

    while (const dirent *entry = readdir(directory))
    {
        const char *filename = entry->d_name;
        constexpr std::size_t VERSION_HEX_LENGTH = 14;
        const std::size_t prefix_length = std::strlen(prefix);
        const std::size_t filename_length = std::strlen(filename);
        if ((std::strncmp(filename, prefix, prefix_length) != 0) ||
            (filename_length != (prefix_length + VERSION_HEX_LENGTH + 4U)) ||
            !has_iop_extension(filename))
        {
            continue;
        }

        std::array<std::uint8_t, 7> version = {};
        bool valid = true;
        for (std::size_t index = 0; index < version.size(); ++index)
        {
            valid = decode_hex_byte(filename[prefix_length + (index * 2U)],
                                    filename[prefix_length + (index * 2U) + 1U],
                                    version[index]);
            if (!valid)
            {
                break;
            }
        }
        if (valid)
        {
            versions.push_back(version);
        }
    }
    closedir(directory);
    return versions;
}

bool erase(std::uint64_t client_name, const std::vector<std::uint8_t> &version_label)
{
    return std::remove(path_for(client_name, version_label).c_str()) == 0;
}

bool erase_all(std::uint64_t client_name)
{
    char prefix[32] = {};
    std::snprintf(prefix, sizeof(prefix), "%08llx_",
                  static_cast<unsigned long long>(client_name & CLIENT_FILE_KEY_MASK));

    DIR *directory = opendir(BASE_PATH);
    if (directory == nullptr)
    {
        return false;
    }

    bool success = true;
    while (const dirent *entry = readdir(directory))
    {
        if ((std::strncmp(entry->d_name, prefix, std::strlen(prefix)) != 0) || !has_iop_extension(entry->d_name))
        {
            continue;
        }

        const std::string path = std::string(BASE_PATH) + "/" + entry->d_name;
        if (std::remove(path.c_str()) != 0)
        {
            success = false;
        }
    }
    closedir(directory);
    return success;
}

bool erase_all()
{
    DIR *directory = opendir(BASE_PATH);
    if (directory == nullptr)
    {
        return false;
    }

    bool success = true;
    while (const dirent *entry = readdir(directory))
    {
        if (!has_iop_extension(entry->d_name))
        {
            continue;
        }

        const std::string path = std::string(BASE_PATH) + "/" + entry->d_name;
        if (std::remove(path.c_str()) != 0)
        {
            success = false;
        }
    }
    closedir(directory);
    return success;
}

std::size_t available_bytes()
{
    std::size_t total = 0;
    std::size_t used = 0;
    if (esp_spiffs_info("storage", &total, &used) != ESP_OK)
    {
        return 0;
    }
    return total >= used ? total - used : 0;
}

Statistics get_statistics()
{
    Statistics result = {};
    esp_spiffs_info("storage", &result.total_bytes, &result.used_bytes);

    DIR *directory = opendir(BASE_PATH);
    if (directory == nullptr)
    {
        return result;
    }
    while (const dirent *entry = readdir(directory))
    {
        if (has_iop_extension(entry->d_name))
        {
            ++result.object_pool_count;
        }
    }
    closedir(directory);
    return result;
}
}
