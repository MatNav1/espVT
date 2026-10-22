#pragma once

#include <cstdint>

namespace section_control
{
enum class GateState : std::uint8_t
{
    Disabled,
    PinsRequired,
    PositionUnavailable,
    PositionStale,
    MinimumFixNotMet,
    CorrectionsStale,
    Ready,
};

struct Snapshot
{
    GateState gate_state = GateState::Disabled;
    std::uint8_t configured_sections = 0;
    bool output_locked = true;
};

bool start();
Snapshot get_snapshot();
bool is_ready();
const char *gate_state_text(GateState state);
}
