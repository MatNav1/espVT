# stb_truetype

`stb_truetype.h` v1.26 from https://github.com/nothings/stb, file as of upstream
commit `6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760` (2024-07-15).
Upstream SHA-256: `ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab`.

Local change: the two `STBTT_memset(pixels, 0, pw*ph)` calls in
`stbtt_BakeFontBitmap_internal()` and `stbtt_PackBegin()` multiply in `size_t`
(`(size_t)pw*ph`), so the size cannot overflow `int` (CodeQL
`cpp/integer-multiplication-cast-to-long`). espVT does not call either function.

Licensed under MIT or public domain (Unlicense), at your choice; see the end of
the header. espVT compiles the implementation in `main/stb_truetype_impl.c` and
uses it in `main/vt_font.cpp` to rasterize Roboto Mono at runtime.
