#include "lwl_app_core.h"
#include "os/os_api.h"

static lwl_app_context_t* s_app_context = NULL;
static OS_MUTEX g_mutex;

static void init_app_context(lwl_app_context_t* context)
{
    memset(s_app_context, 0, sizeof(lwl_app_context_t));
}

lwl_app_context_t* lwl_app_core_context_get_instance()
{
    if (s_app_context == NULL) { 
        os_mutex_pend(&g_mutex, 0);
        if (s_app_context == NULL) {
            s_app_context = (lwl_app_context_t *)malloc(sizeof(lwl_app_context_t));

            init_app_context(s_app_context);
        }
        os_mutex_post(&g_mutex);
    }
    return s_app_context;
}

void lwl_app_core_init()
{
    os_mutex_create(&g_mutex);
}

void lwl_app_core_destroy()
{
    if (s_app_context != NULL) {
        free(s_app_context);
        s_app_context = NULL;
    }
    os_mutex_del(&g_mutex, 1);
}
