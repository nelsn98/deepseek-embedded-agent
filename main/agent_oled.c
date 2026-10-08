#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdatomic.h>
#include "agent_oled.h"
#include "oled_text.h"
#include "oled_font.h"

#define OLED_SCLK 12
#define OLED_MOSI 11
#define OLED_CS   10
#define OLED_DC    9
#define OLED_RST   8
static spi_device_handle_t display;
static uint8_t fb[1024];
static oled_text_t reply_text, notice_text;
static unsigned current_page;
static bool reply_view, ready, rendered_online;
static atomic_bool online;
static esp_err_t io_error;
static char heading[22] = "BOOT";
static void send_bytes(bool data, const uint8_t *bytes, size_t size) {
    if(io_error!=ESP_OK)return;
    io_error=gpio_set_level(OLED_DC,data);
    if(io_error!=ESP_OK){ready=false;return;}
    while (size) {
        size_t n=size>32 ? 32 : size;
        spi_transaction_t t={.length=n*8,.tx_buffer=bytes};
        esp_err_t err=spi_device_polling_transmit(display,&t);
        if(err!=ESP_OK){io_error=err;ready=false;ESP_LOGE("oled","SPI failed: %s",esp_err_to_name(err));return;}
        bytes+=n; size-=n;
    }
}
static void pixel(int x,int y) {
    if(x>=0 && x<128 && y>=0 && y<64)fb[(y/8)*128+x]|=(uint8_t)(1u<<(y%8));
}
static void refresh(void) {
    for(unsigned page=0;page<8;++page) {
        const uint8_t address[]={0xb0|page,0x00,0x10};
        send_bytes(false,address,sizeof(address));
        send_bytes(true,fb+page*128,128);
    }
}
esp_err_t agent_oled_init(void) {
    ESP_LOGI("oled","V4.2 OLED; SSD1306 128x64; SCLK=12 MOSI=11 CS=10 DC=9 RESET=8");
    gpio_config_t pins={.pin_bit_mask=(1ULL<<OLED_DC)|(1ULL<<OLED_RST),.mode=GPIO_MODE_OUTPUT};
    esp_err_t pin_err=gpio_config(&pins);if(pin_err!=ESP_OK)return pin_err;
    spi_bus_config_t bus={.mosi_io_num=OLED_MOSI,.miso_io_num=-1,.sclk_io_num=OLED_SCLK,
                         .quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=32};
    esp_err_t err=spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_DISABLED); if(err!=ESP_OK)return err;
    spi_device_interface_config_t dev={.clock_speed_hz=1000000,.mode=0,.spics_io_num=OLED_CS,.queue_size=1};
    err=spi_bus_add_device(SPI2_HOST,&dev,&display); if(err!=ESP_OK){spi_bus_free(SPI2_HOST);return err;}
    err=gpio_set_level(OLED_RST,1);if(err!=ESP_OK)return err;vTaskDelay(pdMS_TO_TICKS(20));
    err=gpio_set_level(OLED_RST,0);if(err!=ESP_OK)return err;vTaskDelay(pdMS_TO_TICKS(20));
    err=gpio_set_level(OLED_RST,1);if(err!=ESP_OK)return err;vTaskDelay(pdMS_TO_TICKS(100));
    const uint8_t init[]={0xae,0xd5,0x80,0xa8,0x3f,0xd3,0x00,0x40,
        0x8d,0x14,0x20,0x02,0xa1,0xc8,0xda,0x12,0x81,0x3f,
        0xd9,0xf1,0xdb,0x40,0xa4,0xa6,0x2e};
    send_bytes(false,init,sizeof(init));refresh();
    const uint8_t on=0xaf;send_bytes(false,&on,1);
    if(io_error!=ESP_OK)return io_error;
    ESP_LOGI("oled","SPI initialized; no display readback.");
    ready=true; agent_oled_notice("AGENT V4.2", "OLED ready.\nStarting agent..."); return io_error;
}

static void text_row(unsigned row,const char *text,size_t length) {
    for(unsigned i=0;i<21 && i<length;++i) {
        char c=text[i];if(c>='a' && c<='z')c-=32;
        const uint8_t *bits=NULL;
        for(size_t j=0;j<sizeof(glyphs)/sizeof(glyphs[0]);++j)if(glyphs[j].ch==c){bits=glyphs[j].rows;break;}
        if(!bits){for(size_t j=0;j<sizeof(glyphs)/sizeof(glyphs[0]);++j)if(glyphs[j].ch=='?')bits=glyphs[j].rows;}
        for(int y=0;y<7;++y)for(int x=0;x<5;++x)if(bits[y]&(1u<<(4-x)))pixel(i*6+x,row*8+y);
    }
}
static void render(void) {
    if(!ready)return;
    memset(fb,0,sizeof(fb));char title[22];
    if(reply_view)snprintf(title,sizeof(title),"REPLY %u/%u%s%s",current_page+1,reply_text.pages,
        reply_text.non_ascii ? " ?" : "",reply_text.truncated ? " +" : "");
    else snprintf(title,sizeof(title),"%s",heading);
    text_row(0,title,strlen(title));
    const oled_text_t *view=reply_view?&reply_text:&notice_text;
    unsigned page=reply_view?current_page:0;
    for(unsigned row=0;row<OLED_ROWS;++row)text_row(row+1,view->cells[page][row],OLED_COLS);
    rendered_online=atomic_load(&online);
    const char *footer=rendered_online ? "WIFI ON /NEXT /PREV" : "WIFI OFF /NEXT /PREV";
    text_row(7,footer,strlen(footer));refresh();
}
void agent_oled_network(bool connected){atomic_store(&online,connected);}
void agent_oled_poll(void){if(ready && rendered_online!=atomic_load(&online))render();}
void agent_oled_notice(const char *title,const char *text){
    snprintf(heading,sizeof(heading),"%s",title);oled_text_parse(&notice_text,text);reply_view=false;render();
}
void agent_oled_reply(const char *text){oled_text_parse(&reply_text,text);current_page=0;reply_view=true;render();}
void agent_oled_page(int delta){
    if(!reply_text.pages){agent_oled_notice("NO REPLY","Send a question first.");return;}
    if(delta>0 && current_page+1<reply_text.pages)++current_page;
    if(delta<0 && current_page>0)--current_page;
    reply_view=true;render();
}
