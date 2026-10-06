#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
esp_err_t agent_led_init(void);
bool agent_led_set(const char *color, int brightness);
bool agent_led_execute(const char *name, const char *arguments, char *result, size_t cap);
void agent_led_state(char *out, size_t cap);
