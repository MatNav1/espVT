"""Test the renderer's real outline helper without display hardware."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/vt_scene_renderer.cpp").read_text()
function = source[source.index("void draw_outline("):source.index("\nstd::string clipped_text(")]
prefix = r'''
#include <algorithm>
#include <cstdint>
#include <vector>
#include <cassert>
struct Rect {int x,y,w,h;};
namespace lcd_panel {
std::vector<Rect> calls;
void fill_rect(int x,int y,int w,int h,std::uint16_t) {calls.push_back({x,y,w,h});}
}
'''
test = r'''
int main() {
    for (unsigned mask=0; mask<16; ++mask) {
        lcd_panel::calls.clear();
        draw_outline(10,20,100,60,7,2,mask);
        unsigned drawn=0;
        for (auto r:lcd_panel::calls) {
            if (r.w==100) drawn |= (r.y==20 ? 1 : 4);
            else drawn |= (r.x==10 ? 8 : 2);
        }
        assert(drawn==(15^mask));
    }
    lcd_panel::calls.clear();
    draw_outline(0,0,20,20,0,0);
    assert(lcd_panel::calls.empty());
    draw_outline(0,0,20,20,0);
    assert(lcd_panel::calls.size()==4);
}
'''
with tempfile.TemporaryDirectory(prefix="isovt-rectangle-test-") as directory:
    binary = str(Path(directory) / "test")
    subprocess.run(["c++", "-std=c++17", "-x", "c++", "-", "-o", binary],
                   input=prefix + function + test, text=True, check=True)
    subprocess.run([binary], check=True)
print("PASS: 16 suppression combinations, default outline, zero thickness")
