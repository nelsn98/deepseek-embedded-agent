#include "oled_text.h"
#include <string.h>
void oled_text_parse(oled_text_t *out,const char *text) {
    memset(out,0,sizeof(*out));memset(out->cells,' ',sizeof(out->cells));out->pages=1;
    size_t pos=0;bool previous_newline=false;
    const unsigned char *p=(const unsigned char *)(text ? text : "");
    while(*p) {
        unsigned char c=*p++;
        if(c=='\r')continue;
        if(c>=128) {
            out->non_ascii=true;
            /* One placeholder per UTF-8 leading byte; no continuation bytes rendered. */
            if((c&0xc0)==0x80)continue;
            c='?';
        }
        if(c=='\n') {
            /* A full row already wraps; don't add an extra blank row then. */
            if(pos%OLED_COLS || !pos || previous_newline)
                pos+=OLED_COLS-pos%OLED_COLS;
            previous_newline=true;
            continue;
        }
        previous_newline=false;
        if(c=='\t')c=' ';
        if(c<32 || c>126)c='?';
        if(pos>=sizeof(out->cells)){out->truncated=true;break;}
        unsigned page=pos/(OLED_COLS*OLED_ROWS), row=(pos/OLED_COLS)%OLED_ROWS,col=pos%OLED_COLS;
        out->cells[page][row][col]=(char)c;
        out->pages=page+1;
        ++pos;
    }
}
