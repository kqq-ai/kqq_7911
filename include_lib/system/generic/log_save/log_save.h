#ifndef __LOG_SAVE_H
#define __LOG_SAVE_H


//#include "system/includes.h"
//#include "generic/typedef.h"

///#include "cpu.h"


///	-I../../../../include_lib/system/log_save 
///	../../../../include_lib/system/log_save/log_save.c 
#include "lfs.h"
///#include "system/timer.h"

///#define log_save(...)  do {} while (0)
///#define log_save(...)  do {} while (0)
void log_save(int level, const char *tag, const char *format, ...);

///void log_print  (int level, const char *tag, const char *format, ...);
///log_print_save

#endif
