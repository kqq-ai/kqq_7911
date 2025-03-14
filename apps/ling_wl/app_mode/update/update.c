
#include "update.h"
#include "app_mode/lwl_app_framework.h"
#include "app_mode/lwl_app_core.h"
#include "log.h"
#include "app_lfs.h"

static int update_init(void)
{
    return 0;
}

static int update_unint(void)
{
    return 0;
}

void app_update_mode(void)
{
    update_init();

    while (1) {
        // 应用代码
        mdelay(10);
        if (lwl_app_mode_exitting()) {
            update_unint();
            return;
        }
    }
}