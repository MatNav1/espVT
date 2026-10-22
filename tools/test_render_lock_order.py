"""Exercise the real render-request function with concurrent CAN/touch callers.

Run with Python 3 and a host C++ compiler. No ESP hardware is needed.
"""

from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/isobus_terminal_server.cpp").read_text()
start = source.index("bool render_working_set(", source.index("void working_set_render_task("))
function = source[start:source.index("isobus::NAME make_terminal_name()", start)]

prefix = r"""
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
namespace isobus { struct VirtualTerminalServerManagedWorkingSet {}; }
using WS = std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>;
using List = std::vector<WS>;
namespace vt_scene_renderer { bool is_menu_open() { return true; } }
namespace aux_n { void refresh_catalog(const WS &, const List &) {} }
std::mutex render_request_mutex;
std::recursive_mutex list_mutex;
std::atomic<bool> touch_requested_list{false};
thread_local bool touch_thread = false;
struct Server {
    List get_all_working_sets() {
        if (touch_thread) touch_requested_list.store(true);
        std::lock_guard<std::recursive_mutex> lock(list_mutex);
        return {};
    }
    List get_renderable_working_sets() {
        std::lock_guard<std::recursive_mutex> lock(list_mutex);
        return {};
    }
} server;
Server *terminal_server = &server;
void *working_set_render_task_handle = &server;
WS pending_render_working_set;
List pending_render_working_sets, pending_auxiliary_working_sets;
std::atomic<int> notifications{0};
void xTaskNotifyGive(void *) { ++notifications; }
"""

suffix = r"""
int main() {
    auto ws = std::make_shared<isobus::VirtualTerminalServerManagedWorkingSet>();
    std::unique_lock<std::recursive_mutex> can_callback_lock(list_mutex);
    std::thread touch([&] { touch_thread = true; render_working_set(ws); });
    while (!touch_requested_list.load()) std::this_thread::yield();
    // CAN repaint callback is invoked with the working-set list locked.
    render_working_set(ws);
    can_callback_lock.unlock();
    touch.join();
    return notifications == 2 ? 0 : 1;
}
"""

with tempfile.TemporaryDirectory(prefix="isovt-lock-test-") as directory:
    directory = Path(directory)
    for name, body in [("fixed", function), ("old-order", function.replace(
        "    // CAN repaint callbacks", "    std::unique_lock<std::mutex> old_lock(render_request_mutex);\n    // CAN repaint callbacks"
    ).replace("        std::lock_guard<std::mutex> lock(render_request_mutex);", ""))]:
        cpp = directory / f"{name}.cpp"
        executable = directory / name
        cpp.write_text(prefix + body + suffix)
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-pthread",
                        str(cpp), "-o", str(executable)], check=True)
        try:
            subprocess.run([str(executable)], check=True, timeout=3)
        except subprocess.TimeoutExpired:
            if name == "fixed":
                raise AssertionError("Render request deadlocked")
            print("PASS: old lock order reproduces the deadlock")
        else:
            if name == "old-order":
                raise AssertionError("Negative control did not reproduce the deadlock")
            print("PASS: concurrent CAN repaint and touch render finish")
