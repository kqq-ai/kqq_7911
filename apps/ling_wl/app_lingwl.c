#include "app_config.h"
#include "app_lingwl.h"

#include "cpu.h"
#include "app_core.h"
#include "action.h"

static void app_lingwl_main(void)
{
    printf("Hello World!\r\n");
}

static int app_lingwl_event_handler(struct application *app, struct sys_event *event)
{
    switch (event->type) {
    case SYS_KEY_EVENT:
        // return app_music_key_event_handler((struct key_event *)event->payload);
    case SYS_DEVICE_EVENT:
        // return app_music_device_event_handler(event);
#ifdef CONFIG_NET_ENABLE
    case SYS_NET_EVENT:
        // return app_music_net_event_handler((struct net_event *)event->payload);
#endif
#ifdef CONFIG_BT_ENABLE
    case SYS_BT_EVENT:
        // return app_music_bt_event_handler(event);
#endif
    default:
        return false;
    }
}

static int app_lingwl_state_machine(struct application *app, enum app_state state,
                                   struct intent *it)
{
    switch (state) {
    case APP_STA_CREATE:
        break;
    case APP_STA_START:
        if (!it) {
            break;
        }
        switch (it->action) {
        case ACTION_LING_WL_MAIN:
            app_lingwl_main();
            break;
        }
        break;
    case APP_STA_PAUSE:
        break;
    case APP_STA_RESUME:
        break;
    case APP_STA_STOP:
        break;
    case APP_STA_DESTROY:
        break;
    }

    return 0;
}

static const struct application_operation app_lingwl_ops = {
    .state_machine  = app_lingwl_state_machine,
    .event_handler 	= app_lingwl_event_handler,
};

REGISTER_APPLICATION(app_lingwl) = {
    .name 	= "app_lingwl",
    .ops 	= &app_lingwl_ops,
    .state  = APP_STA_DESTROY,
};
