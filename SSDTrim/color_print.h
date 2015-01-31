#ifndef __COLOR_PRINT_H__
#define __COLOR_PRINT_H__

int color_printf(unsigned short color, const char * format, ...);

#define PRINT_ERR(f, ...) color_printf(FOREGROUND_RED | FOREGROUND_INTENSITY, f, __VA_ARGS__)
#define PRINT_WRN(f, ...) color_printf(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY, f, __VA_ARGS__)
#define PRINT_INFO(f, ...) color_printf(FOREGROUND_GREEN | FOREGROUND_INTENSITY, f, __VA_ARGS__)

#endif
