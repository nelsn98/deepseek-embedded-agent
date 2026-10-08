#pragma once
#include <stdbool.h>
#include "esp_err.h"
esp_err_t agent_oled_init(void);
void agent_oled_network(bool online); /* Event-loop safe: no SPI calls. */
void agent_oled_poll(void); /* Call only from main task. */
void agent_oled_notice(const char *title,const char *text);
void agent_oled_reply(const char *text);
void agent_oled_page(int delta);
