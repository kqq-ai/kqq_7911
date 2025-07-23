#ifndef __WSS_CLIENT_H__
#define __WSS_CLIENT_H__

#include "cpu.h"
#include "web_socket/websocket_api.h"

/**
 * @brief Callback function type for receiving WebSocket data
 * @param buf Pointer to the received data buffer
 * @param len Length of the received data
 * @param type Type of the WebSocket frame
 */
typedef void (*user_wss_recv_cb_t)(u8 *buf, u32 len, u8 type);

/**
 * @brief Establishes a WebSocket connection to a server
 * @param url The WebSocket server URL to connect to (e.g., "wss://example.com")
 * @param websockets_callback Callback function to handle incoming data
 * @return Pointer to the websocket_struct on success, NULL on failure
 */
struct websocket_struct *wss_server_connect(char *url);

/**
 * @brief Sends data through an established WebSocket connection
 * @param websockets_info Pointer to the websocket_struct obtained from wss_server_connect()
 * @param buf Pointer to the data buffer to send
 * @param size Size of the data to send
 * @return Number of bytes sent on success, negative value on error
 */
int  wss_server_send(struct websocket_struct *websockets_info, u8 *buf, int size);

/**
 * @brief Sends binary data through an established WebSocket connection
 * @param websockets_info Pointer to the websocket_struct obtained from wss_server_connect()
 * @param buf Pointer to the data buffer to send
 * @param size Size of the data to send
 * @return Number of bytes sent on success, negative value on error
 */
int  wss_server_send_bin(struct websocket_struct *websockets_info, u8 *buf, int size);

/**
 * @brief Closes an established WebSocket connection
 * @param websockets_info Pointer to the websocket_struct obtained from wss_server_connect()
 * @note This function will free all resources associated with the connection
 */
void wss_server_disconnect(struct websocket_struct *websockets_info);

#endif