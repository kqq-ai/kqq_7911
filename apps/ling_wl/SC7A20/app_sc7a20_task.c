#include "app_sc7a20_task.h"
#include "app_config.h"
#include "system/includes.h"
#include "device/device.h"
#include "gpio.h"
#include "uart.h"
#include "log.h"
#include "sc7a20.h"

void * g_iic_hdl = NULL;



static void app_sc7a20_task(void * arg)
{
    printf("app_sc7a20_task !\r\n");
    /* 1.获取全局的iic 句柄 */

    while (ERASE_INTRRUPT_RNABLE)
    {
        /* 2.每隔一段时间检测中断是否发生 */
        if (sc7a20_interrupt_detect(g_iic_hdl))
        {
            /* 3.如果发生中断，则处理 */
            sc7a20_clear_interrupt(g_iic_hdl);
        }
        mdelay(ERASE_INTRRUPT_TIME);
    }
    
}



void * app_sc7a20_init(void)
{
    void * iic_hdl = sc7a20_init(IIC_NAME);

    g_iic_hdl = iic_hdl;

    if (iic_hdl == NULL) {
        printf("app_sc7a20_init 陀螺仪 初始化 failed! Task not created.\n");
        return NULL;
    }

    os_task_create(app_sc7a20_task, NULL, 10, 2048, 0, "app_sc7a20_task");

    return g_iic_hdl;

    
}