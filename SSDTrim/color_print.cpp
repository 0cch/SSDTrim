#include "color_print.h"

#include <stdio.h>
#include <Windows.h>

int color_printf(unsigned short color, const char * format, ...)
{
	HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);

	CONSOLE_SCREEN_BUFFER_INFO console_info = {0};
	if (h != NULL) {
		GetConsoleScreenBufferInfo(h, &console_info);
	}

	SetConsoleTextAttribute(h, color);

	va_list v;
	va_start(v, format);
	int retval = vprintf(format, v);
	va_end(v);
	
	SetConsoleTextAttribute(h, console_info.wAttributes);

	return retval;
}