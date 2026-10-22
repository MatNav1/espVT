#pragma once

#include "esp_err.h"

namespace can_monitor
{
esp_err_t start();
void enable_ui_updates();
bool bus_activity_detected();
bool request_address_claims();
}
