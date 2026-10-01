#ifndef _GUNCON2_UTILS_H
#define _GUNCON2_UTILS_H
#include "PS2VM.h"

#define MAX_GUNS 2 // can be 1 or 2

struct lightgun_info_s {
    char serial[10];
    int width;
    int height;    
    int scale_x;
    int scale_y;
    int center_x;
    int center_y;
};

extern const struct lightgun_info_s* g_lightgun_info;
bool load_gun_info(const char* gameName);
void register_guncon2(CPS2VM* vm, int instance);
void guncon2_set_state(CPS2VM* vm, int instance, uint32 buttons, int32 x, int32 y, bool offscreen);
#endif