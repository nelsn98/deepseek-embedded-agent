#include "agent_led.h"
#include "led_strip.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static led_strip_handle_t strip;
static const char *current_color = "unknown";
static int current_brightness;
typedef struct { const char *name; unsigned r, g, b; } color_t;
static const color_t colors[] = {
    {"off",0,0,0}, {"red",1,0,0}, {"green",0,1,0}, {"blue",0,0,1},
    {"yellow",1,1,0}, {"cyan",0,1,1}, {"purple",1,0,1}, {"white",1,1,1}
};

bool agent_led_set(const char *color, int brightness) {
    if (!strip || !color || brightness < 0 || brightness > 100) return false;
    for (size_t i = 0; i < sizeof(colors)/sizeof(colors[0]); ++i) {
        if (strcmp(color, colors[i].name)) continue;
        /* Limit each channel to 32/255 even at requested brightness=100. */
        unsigned level = (32u * (unsigned)brightness + 50u) / 100u;
        esp_err_t err = led_strip_set_pixel(strip, 0,
            colors[i].r * level, colors[i].g * level, colors[i].b * level);
        if (err == ESP_OK) err = led_strip_refresh(strip);
        if (err != ESP_OK) { current_color = "unknown"; return false; }
        current_color = level == 0 ? "off" : colors[i].name;
        current_brightness = !strcmp(current_color, "off") ? 0 : brightness;
        return true;
    }
    return false;
}

esp_err_t agent_led_init(void) {
    led_strip_config_t config = {
        .strip_gpio_num = 38, .max_leds = 1, .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10000000,
        .mem_block_symbols = 64, .flags.with_dma = true,
    };
    esp_err_t err = led_strip_new_rmt_device(&config, &rmt, &strip);
    if (err != ESP_OK) return err;
    return agent_led_set("off", 0) ? ESP_OK : ESP_FAIL;
}

void agent_led_state(char *out, size_t cap) {
    snprintf(out, cap, "color=%s brightness=%d (commanded state, no optical feedback)",
             current_color, current_brightness);
}

bool agent_led_execute(const char *name, const char *arguments, char *result, size_t cap) {
    bool ok = false;
    const char *error = "invalid_arguments";
    cJSON *args = NULL;
    if (!name || strcmp(name, "set_led")) { error = "unknown_tool"; goto done; }
    if (!arguments || strlen(arguments) > 256) goto done;
    args = cJSON_ParseWithOpts(arguments, NULL, true);
    if (!cJSON_IsObject(args)) goto done;
    /* Exactly two keys: reject duplicate and unrecognised fields too. */
    unsigned fields = 0, color_fields = 0, brightness_fields = 0;
    const cJSON *field;
    cJSON_ArrayForEach(field, args) {
        ++fields;
        if (field->string && !strcmp(field->string, "color")) ++color_fields;
        if (field->string && !strcmp(field->string, "brightness")) ++brightness_fields;
    }
    if (fields != 2 || color_fields != 1 || brightness_fields != 1) goto done;
    cJSON *color = cJSON_GetObjectItemCaseSensitive(args, "color");
    cJSON *brightness = cJSON_GetObjectItemCaseSensitive(args, "brightness");
    if (!cJSON_IsString(color) || !cJSON_IsNumber(brightness)) goto done;
    double b = brightness->valuedouble;
    if (!isfinite(b) || b < 0 || b > 100 || b != (int)b) goto done;
    bool known = false;
    for (size_t i = 0; i < sizeof(colors)/sizeof(colors[0]); ++i)
        if (!strcmp(color->valuestring, colors[i].name)) known = true;
    if (!known) goto done;
    ok = agent_led_set(color->valuestring, (int)b);
    error = "led_driver_failed";
done:
    if (ok) snprintf(result, cap, "{\"ok\":true,\"color\":\"%s\",\"brightness\":%d,\"gpio\":38}",
                     current_color, current_brightness);
    else snprintf(result, cap, "{\"ok\":false,\"error\":\"%s\"}", error);
    cJSON_Delete(args);
    return ok;
}
