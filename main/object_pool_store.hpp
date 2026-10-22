#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace object_pool_store
{
struct Statistics
{
    std::size_t total_bytes = 0;
    std::size_t used_bytes = 0;
    std::size_t object_pool_count = 0;
};

bool init();
bool save(std::uint64_t client_name, const std::vector<std::uint8_t> &version_label, const std::vector<std::uint8_t> &data);
std::vector<std::uint8_t> load(std::uint64_t client_name, const std::vector<std::uint8_t> &version_label);
std::vector<std::array<std::uint8_t, 7>> list_versions(std::uint64_t client_name);
bool erase(std::uint64_t client_name, const std::vector<std::uint8_t> &version_label);
bool erase_all(std::uint64_t client_name);
bool erase_all();
std::size_t available_bytes();
Statistics get_statistics();
}
