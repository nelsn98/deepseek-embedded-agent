#pragma once
#include <stdbool.h>
#include <stddef.h>
#define OLED_COLS 21
#define OLED_ROWS 6
#define OLED_PAGES 32
typedef struct { char cells[OLED_PAGES][OLED_ROWS][OLED_COLS]; unsigned pages; bool truncated, non_ascii; } oled_text_t;
void oled_text_parse(oled_text_t *out, const char *text);
