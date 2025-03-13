#ifndef _THREADPOOL_TASKS_H_
#define _THREADPOOL_TASKS_H_

#include "threadpool.h"
#include "app_lfs.h"

/* 根据 role名字 来保存 opening文件 */
typedef struct {
    char * url;
    int role_name;
    char opening_filename[MAX_OPENING_FILE_LEN];
    lfs_t *lfs;
} save_role_opening_task_param_t;

void save_role_opening_file_task(void * args);  /* sprintf(filename, "opening_%d.mp3", name); */



#endif