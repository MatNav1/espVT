// Single compilation unit for the stb_truetype implementation used by vt_font.
#include <stdlib.h>

#include "esp_heap_caps.h"

// Glyph outlines and rasterizer scratch memory go to PSRAM when available.
#define STBTT_malloc(size, user) ((void)(user), heap_caps_malloc_prefer((size), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT))
#define STBTT_free(pointer, user) ((void)(user), heap_caps_free(pointer))
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
