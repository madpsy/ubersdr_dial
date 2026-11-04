#pragma once
#include <lvgl.h>

// Montserrat with accented Latin (U+00A0-017F) and typographic punctuation,
// falling back to the built-in font of the same size for ASCII and symbols.
// Use for text that comes from outside (receiver names and locations, the
// page's name): the built-in fonts are ASCII only and show boxes otherwise.
extern lv_font_t font_latin_12, font_latin_14, font_latin_16, font_latin_20;
