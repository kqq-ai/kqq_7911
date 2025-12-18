#ifndef __BLE_OPUS_STREAM_RX_H__
#define __BLE_OPUS_STREAM_RX_H__

#include <stdint.h>

#include "app_config.h"
#include "net_music_play_my.h"

/**
 * Bind the music_player instance that will be used for decoding/playback.
 * Must be called once after music_player_init().
 */
void ble_opus_stream_rx_bind_player(music_player *player);

/** Called when BLE GATT server connects/disconnects (server role). */
void ble_opus_stream_rx_on_connect(uint16_t conn_handle);
void ble_opus_stream_rx_on_disconnect(uint16_t conn_handle);

/** Called from ATT write callback with Opus stream bytes. */
int ble_opus_stream_rx_on_audio_write(const uint8_t *data, uint16_t len);

#endif /* __BLE_OPUS_STREAM_RX_H__ */


