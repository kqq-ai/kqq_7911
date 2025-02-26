#include "lwl_app_core.h"
#include "os/os_api.h"

static lwl_app_context_t* s_app_context = NULL;
static OS_MUTEX g_mutex;

static void init_app_context(lwl_app_context_t* context)
{
    memset(s_app_context, 0, sizeof(lwl_app_context_t));
    s_app_context->active_eend_idx = -1;
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

lwl_eend_dev_t* find_eend_dev_active()
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    return (context->active_eend_idx == -1) ? NULL : &context->eend_dev_list[context->active_eend_idx];
}

lwl_eend_dev_t* find_eend_dev_by_sn(u8 *sn)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    for (int i = 0; i < EEND_MAX_CNT; ++i) {
        if (memcmp(context->eend_dev_list[i].sn, sn, SN_SIZE) == 0) {
            return &context->eend_dev_list[i];
        }
    }
    return NULL;
}

void set_eend_dev_active(lwl_eend_dev_t *pdev)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    for (int i = 0; i < EEND_MAX_CNT; ++i) {
        if (&context->eend_dev_list[i] == pdev) {
            context->active_eend_idx = i;
            break;
        }
    }
}