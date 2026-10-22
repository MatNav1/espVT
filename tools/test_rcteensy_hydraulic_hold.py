"""Host tests for the actual RCteensy timeout/event functions, without hardware."""
import pathlib
import subprocess
import sys
import tempfile

source = pathlib.Path(sys.argv[1]).read_text()

def function(name):
    start = source.index("void " + name + "(")
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

harness = r'''
#include <cstdint>
#include <cassert>
namespace isobus { struct VirtualTerminalClient {
enum class KeyActivationCode {ButtonUnlatchedOrReleased, ButtonPressedOrLatched, ButtonStillHeld, ButtonPressAborted};
bool connected = true;
bool get_is_connected() { return connected; }
}; }
using Key = isobus::VirtualTerminalClient::KeyActivationCode;
uint32_t clockMs = 0;
uint32_t millis() { return clockMs; }
struct SerialStub {
operator bool() { return false; }
int availableForWrite() { return 0; }
template<class... T> void printf(const char*, T...) {}
} Serial;
constexpr uint8_t VT_HYDRAULIC_FUNCTION_COUNT = 14;
constexpr uint32_t VT_HYDRAULIC_HOLD_TIMEOUT_MS = 1000;
bool VT_HydraulicMomentaryActive[14] = {};
bool VT_HydraulicLatched[14] = {};
bool VT_HydraulicBistablePressSeen[14] = {};
uint32_t VT_HydraulicMomentaryLastEventMs[14] = {};
struct { bool HydraulicFunctionEnabled[14] = {}; bool HydraulicFunctionBistable[14] = {}; } Machine;
isobus::VirtualTerminalClient terminal;
auto *ISOBUSVirtualTerminal = &terminal;
unsigned outputUpdates = 0;
void VT_UpdateHydraulicRelays() { ++outputUpdates; }
bool VT_HydraulicControlsEnabled() { return true; }
'''
harness += function("VT_SetHydraulicFunction")
harness += function("VT_ServiceMomentaryHydraulicTimeout")
harness += function("VT_HandleHydraulicFunctionEvent")
harness += r'''
int main() {
 for (auto &enabled : Machine.HydraulicFunctionEnabled) enabled = true;
 auto event = [](Key key) { VT_HandleHydraulicFunctionEvent(0, key); };
 event(Key::ButtonPressedOrLatched);
 clockMs = 20; event(Key::ButtonUnlatchedOrReleased);
 assert(!VT_HydraulicMomentaryActive[0]);
 event(Key::ButtonStillHeld); assert(!VT_HydraulicMomentaryActive[0]);
 event(Key::ButtonPressedOrLatched);
 clockMs = 1019; VT_ServiceMomentaryHydraulicTimeout();
 assert(VT_HydraulicMomentaryActive[0]);
 clockMs = 1020; VT_ServiceMomentaryHydraulicTimeout();
 assert(!VT_HydraulicMomentaryActive[0]);
 event(Key::ButtonStillHeld); assert(!VT_HydraulicMomentaryActive[0]);
 event(Key::ButtonPressedOrLatched);
 for (int i=0; i<50; ++i) { clockMs += 200; event(Key::ButtonStillHeld); assert(VT_HydraulicMomentaryActive[0]); }
 event(Key::ButtonPressAborted); assert(!VT_HydraulicMomentaryActive[0]);
 event(Key::ButtonPressedOrLatched);
 terminal.connected = false; VT_ServiceMomentaryHydraulicTimeout();
 assert(!VT_HydraulicMomentaryActive[0]);
 terminal.connected = true; event(Key::ButtonStillHeld);
 assert(!VT_HydraulicMomentaryActive[0]);
 clockMs = UINT32_MAX - 400; event(Key::ButtonPressedOrLatched);
 clockMs += 999; VT_ServiceMomentaryHydraulicTimeout(); assert(VT_HydraulicMomentaryActive[0]);
 ++clockMs; event(Key::ButtonStillHeld); assert(!VT_HydraulicMomentaryActive[0]);
 Machine.HydraulicFunctionBistable[1] = true;
 VT_HandleHydraulicFunctionEvent(1, Key::ButtonPressedOrLatched);
 VT_HandleHydraulicFunctionEvent(1, Key::ButtonUnlatchedOrReleased);
 clockMs += 2000; VT_ServiceMomentaryHydraulicTimeout(); assert(VT_HydraulicLatched[1]);
 VT_HandleHydraulicFunctionEvent(1, Key::ButtonPressedOrLatched); assert(!VT_HydraulicLatched[1]);
 Machine.HydraulicFunctionEnabled[0] = false;
 event(Key::ButtonPressedOrLatched); assert(!VT_HydraulicMomentaryActive[0]);
 VT_HandleHydraulicFunctionEvent(255, Key::ButtonPressedOrLatched);
 assert(outputUpdates > 0);
}
'''
with tempfile.TemporaryDirectory(prefix="rcteensy-hold-test-") as directory:
    binary = str(pathlib.Path(directory) / "test")
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-x", "c++", "-", "-o", binary], input=harness, text=True, check=True)
    subprocess.run([binary], check=True)
print("PASS: release, abort, lost release, continuous hold, late hold, disconnect, timer wrap, bistable, disabled, invalid index")
