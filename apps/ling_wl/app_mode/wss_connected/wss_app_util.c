#include "wss_app_util.h"

#include "wss/wss_client.h"
#include "protocol/lingos/loswssp.h"

int communication_talk_message(struct websocket_struct *websockets_info, char *content)
{
    // char *send_data = NULL;
    // loswssp_create_message_event_message_text("abcdefg-123456", content, &send_data);
    // if (FALSE == wss_server_send(websockets_info, send_data, strlen(send_data))) {
    //     log_e("Error %s %d.", __FILE__, __LINE__);
    //     loswssp_destroy_message(send_data);
    //     return -1;
    // }
    // log_i(send_data);
    // loswssp_destroy_message(send_data);
    return 0;
}

int communication_init_role(struct websocket_struct *websockets_info, int ip_id)
{
    // char *send_data = NULL;
    // loswssp_create_message_event_session("abcdefg-123456", 
    //     ip_id, 
    //     "esp32-testss", 
    //     true, 
    //     &send_data);
    // if (FALSE == wss_server_send(websockets_info, send_data, strlen(send_data))) {
    //     log_e("Error %s %d.", __FILE__, __LINE__);
    //     loswssp_destroy_message(send_data);
    //     return -1;
    // }
    // log_i(send_data);
    // loswssp_destroy_message(send_data);
    return 0;
}

int wss_2ip_switch(struct websocket_struct *websockets_info, int session_id, int ip_id)
{
    // char *send_data = NULL;
    // loswssp_create_message_event_session_switch("abcdefg-123456", 
    //     session_id, 
    //     ip_id, 
    //     "esp32-testss", 
    //     true, 
    //     true, 
    //     &send_data);
    // if (FALSE == wss_server_send(websockets_info, send_data, strlen(send_data))) {
    //     log_e("Error %s %d.", __FILE__, __LINE__);
    //     loswssp_destroy_message(send_data);
    //     return -1;
    // }
    // log_i(send_data);
    // loswssp_destroy_message(send_data);
    return 0;
}

int communication_message_asr(struct websocket_struct *websockets_info)
{
    // char *send_data = NULL;
    // loswssp_create_message_event_message_asr("abcdefg-123456", &send_data);
    // if (FALSE == wss_server_send(websockets_info, send_data, strlen(send_data))) {
    //     log_e("Error %s %d.", __FILE__, __LINE__);
    //     loswssp_destroy_message(send_data);
    //     return -1;
    // }
    // log_i(send_data);
    // loswssp_destroy_message(send_data);
    return 0;
}

int communication_message_asr_start(struct websocket_struct *websockets_info)
{
    // char *send_data = NULL;
    // loswssp_create_message_event_message_asr_start("abcdefg-123456", &send_data);
    // if (FALSE == wss_server_send(websockets_info, send_data, strlen(send_data))) {
    //     log_e("Error %s %d.", __FILE__, __LINE__);
    //     loswssp_destroy_message(send_data);
    //     return -1;
    // }
    // log_i(send_data);
    // loswssp_destroy_message(send_data);
    return 0;
}

int communication_message_asr_end(struct websocket_struct *websockets_info)
{
    // char *send_data = NULL;
    // loswssp_create_message_event_message_asr_end("abcdefg-123456", &send_data);
    // if (FALSE == wss_server_send(websockets_info, send_data, strlen(send_data))) {
    //     log_e("Error %s %d.", __FILE__, __LINE__);
    //     loswssp_destroy_message(send_data);
    //     return -1;
    // }
    // log_i(send_data);
    // loswssp_destroy_message(send_data);
    return 0;
}

int communication_message_asr_data(struct websocket_struct *websockets_info, char *data_base64)
{
    // char *send_data = NULL;
    // loswssp_create_message_event_message_asr_data("abcdefg-123456", data_base64, &send_data);
    // if (FALSE == wss_server_send(websockets_info, send_data, strlen(send_data))) {
    //     log_e("Error %s %d.", __FILE__, __LINE__);
    //     loswssp_destroy_message(send_data);
    //     return -1;
    // }
    // loswssp_destroy_message(send_data);
    return 0;
}

int communication_message_stop(struct websocket_struct *websockets_info)
{
    // char *send_data = NULL;
    // loswssp_create_message_event_message_stop("abcdefg-123456", &send_data);
    // if (FALSE == wss_server_send(websockets_info, send_data, strlen(send_data))) {
    //     log_e("Error %s %d.", __FILE__, __LINE__);
    //     loswssp_destroy_message(send_data);
    //     return -1;
    // }
    // log_i(send_data);
    // loswssp_destroy_message(send_data);
    return 0;
}

int wss_ping_send(struct websocket_struct *websockets_info)
{
    // char *send_data = NULL;
    // loswssp_create_message_event_ping("abcdefg-123456", &send_data);
    // if (FALSE == wss_server_send(websockets_info, send_data, strlen(send_data))) {
    //     log_e("Error %s %d.", __FILE__, __LINE__);
    //     loswssp_destroy_message(send_data);
    //     return -1;
    // }
    // log_i(send_data);
    // loswssp_destroy_message(send_data);
    return 0;
}
