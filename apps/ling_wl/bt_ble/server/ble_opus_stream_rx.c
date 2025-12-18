#include "ble_opus_stream_rx.h"

#if defined(CONFIG_BLE_OPUS_STREAM_RX)

#include "server/audio_server.h"
#include "server/server_core.h"
#include "log.h"
#include "util/ring_queue.h"
#include "timer.h"
#include <string.h>

// -------------------- internal state --------------------
static music_player *s_player = NULL;
static volatile uint8_t s_connected = 0;
static volatile uint8_t s_playing = 0;

// -------------------- BLE Opus framing --------------------
/*
 * Optional framing to avoid "restart mid-stream" corruption and to support finite file playback:
 * Sender sends 8-byte header before payload:
 *   u32 magic     = 0x55504F42 ('B''O''P''U' as little-endian u32)
 *   u32 total_len = payload byte length (little-endian)
 *
 * If header is not present (legacy sender), we fall back to raw stream mode.
 */
#define BLE_OPUS_HDR_MAGIC_LE   (0x55504F42u) // 'B''O''P''U'
typedef struct __attribute__((packed)) {
    uint32_t magic_le;
    uint32_t total_len_le;
} ble_opus_hdr_t;

static uint8_t  s_hdr_buf[sizeof(ble_opus_hdr_t)];
static uint8_t  s_hdr_fill = 0;
static uint8_t  s_have_header = 0;   // 0: waiting/unknown, 1: decided
static uint8_t  s_framed = 0;        // 1 when magic header matched
static uint32_t s_expect_len = 0;    // valid when framed
static uint32_t s_got_len = 0;       // payload bytes accepted into queue
static uint8_t  s_eof = 0;           // framed and got_len==expect_len
static uint8_t  s_eof_reported = 0;  // VFS returned EOS due to eof, block auto-restart

static uint32_t s_rx_bytes = 0;
static uint32_t s_rx_pkts = 0;
static uint32_t s_drop_full = 0;
static uint32_t s_last_stat_ms = 0;
static uint32_t s_last_rx_ms = 0;
static uint32_t s_ignored_pkts = 0;
static uint32_t s_ignored_bytes = 0;
static int s_last_dec_status = -999;

// VFS/read-side stats
static uint32_t s_fread_calls = 0;
static uint32_t s_fread_timeouts = 0;
static uint32_t s_fread_disc_eos = 0;
static uint32_t s_fread_err = 0;
static uint32_t s_starve_spins = 0;

// -------------------- opus stream VFS --------------------
static int ble_opus_vfs_fread(void *file, void *buf, u32 len)
{
    RingQueue *q = (RingQueue *)file;
    uint32_t got = 0;
    uint32_t start_ms = timer_get_ms();

    s_fread_calls++;
#if defined(CONFIG_BLE_OPUS_STREAM_RX_DEBUG) && (CONFIG_BLE_OPUS_STREAM_RX_DEBUG)
    if (s_fread_calls <= 3 || (s_fread_calls % 200) == 0) {
        int qsz = (q ? ring_queue_size(q) : -1);
        log_i("[BLE_OPUS][VFS] fread#%u req=%u qsz=%d playing=%u connected=%u",
              s_fread_calls, (unsigned)len, qsz, s_playing, s_connected);
    }
#endif

    while (got < len) {
        if (!s_connected) {
            s_fread_disc_eos++;
#if defined(CONFIG_BLE_OPUS_STREAM_RX_DEBUG) && (CONFIG_BLE_OPUS_STREAM_RX_DEBUG)
            log_w("[BLE_OPUS][VFS] EOS: disconnected while reading (req=%u got=%u)",
                  (unsigned)len, (unsigned)got);
#endif
            return 0; // EOS on disconnect
        }
        // framed EOS: once payload is fully received and queue is drained, signal EOS
        if (s_framed && s_eof && (ring_queue_size(q) <= 0)) {
            s_eof_reported = 1;
            return 0;
        }
        int r = ring_queue_try_read_timeout(q, (char *)buf + got, (int)(len - got), 200);
        if (r == RINGQUEUE_Is_Empty) {
            s_starve_spins++;
            if ((int)(timer_get_ms() - start_ms) > CONFIG_BLE_OPUS_STREAM_READ_TIMEOUT_MS) {
                s_fread_timeouts++;
#if defined(CONFIG_BLE_OPUS_STREAM_RX_DEBUG) && (CONFIG_BLE_OPUS_STREAM_RX_DEBUG)
                int qsz = ring_queue_size(q);
                log_w("[BLE_OPUS][VFS] EOS: starvation timeout=%dms (req=%u got=%u qsz=%d rx_age=%ums)",
                      (int)CONFIG_BLE_OPUS_STREAM_READ_TIMEOUT_MS,
                      (unsigned)len, (unsigned)got, qsz,
                      (unsigned)(timer_get_ms() - s_last_rx_ms));
#endif
                return 0; // EOS on prolonged starvation
            }
            continue;
        }
        if (r < 0) {
            s_fread_err++;
#if defined(CONFIG_BLE_OPUS_STREAM_RX_DEBUG) && (CONFIG_BLE_OPUS_STREAM_RX_DEBUG)
            log_e("[BLE_OPUS][VFS] fread error=%d (req=%u got=%u)", r, (unsigned)len, (unsigned)got);
#endif
            return 0;
        }
        got += (uint32_t)r;
    }

    return (int)got;
}

static int ble_opus_vfs_fseek(void *file, u32 offset, int origin)
{
    (void)file;
    (void)offset;
    (void)origin;
    return -1;
}

static int ble_opus_vfs_flen(void *file)
{
    (void)file;
    return -1;
}

static const struct audio_vfs_ops ble_opus_audio_dec_vfs_ops = {
    .fread = ble_opus_vfs_fread,
    .fseek = ble_opus_vfs_fseek,
    .flen  = ble_opus_vfs_flen,
};

// -------------------- decoder ops --------------------
static int ble_opus_get_dec_status(music_player *player)
{
    union audio_req req = {0};
    if (!player || !player->dec_server) {
        return -1;
    }
    req.dec.cmd = AUDIO_DEC_GET_STATUS;
    server_request(player->dec_server, AUDIO_REQ_DEC, &req);
    return req.dec.status;
}

static int ble_opus_dec_stop(music_player *player)
{
    union audio_req req = {0};
    if (player && player->dec_server) {
        req.dec.cmd = AUDIO_DEC_STOP;
        server_request(player->dec_server, AUDIO_REQ_DEC, &req);
    }
    if (player) {
        ring_queue_clear(&player->queue);
        player->cmp_file = NULL;
    }
    s_playing = 0;
    return 0;
}

static const struct music_dec_ops ble_opus_stream_dec_ops = {
    .dec_file   = NULL,
    .dec_stop   = ble_opus_dec_stop,
    .dec_status = ble_opus_get_dec_status,
};

static int ble_opus_try_start_decode(void)
{
    if (!s_player || !s_player->dec_server) {
#if defined(CONFIG_BLE_OPUS_STREAM_RX_DEBUG) && (CONFIG_BLE_OPUS_STREAM_RX_DEBUG)
        if (!s_player) {
            log_w("[BLE_OPUS] start_decode: no player bound");
        } else {
            log_w("[BLE_OPUS] start_decode: player bound but dec_server=NULL (player=%p)", s_player);
        }
#endif
        return -1;
    }
    if (s_playing) {
        return 0;
    }
    if (!s_connected) {
        return -1;
    }
    // Don't auto-restart decode after we already ended a framed session.
    // Wait for a new header (new session) to reset s_eof_reported.
    if (s_framed && s_eof_reported) {
        return 0;
    }

    int qsz = ring_queue_size(&s_player->queue);
    if (qsz < 0) {
        return -1;
    }
    int need = (int)CONFIG_BLE_OPUS_STREAM_PREBUF_BYTES;
    if (s_framed && s_expect_len > 0 && (int)s_expect_len < need) {
        need = (int)s_expect_len; // short file: wait for full payload
    }
    if (qsz < need) {
        return 0;
    }

    // stop any previous decode (best effort)
    if (s_player->dec_ops && AUDIO_DEC_STOP != s_player->dec_ops->dec_status(s_player)) {
#if defined(CONFIG_BLE_OPUS_STREAM_RX_DEBUG) && (CONFIG_BLE_OPUS_STREAM_RX_DEBUG)
        int st = s_player->dec_ops->dec_status(s_player);
        log_w("[BLE_OPUS] start_decode: stopping previous decode (status=%d)", st);
#endif
        s_player->dec_ops->dec_stop(s_player);
    }

    union audio_req req = {0};
    req.dec.cmd             = AUDIO_DEC_OPEN;
    req.dec.volume          = s_player->volume;
    req.dec.output_buf      = NULL;
    req.dec.output_buf_len  = 4 * 1024;
    req.dec.file            = (void *)&s_player->queue;
    req.dec.dec_type        = "opus";
    req.dec.channel         = 2;
    req.dec.sample_rate     = CONFIG_BLE_OPUS_STREAM_SAMPLE_RATE;
    req.dec.priority        = 1;
    req.dec.vfs_ops         = &ble_opus_audio_dec_vfs_ops;
    req.dec.sample_source   = "dac";

#if defined(CONFIG_BLE_OPUS_STREAM_RX_DEBUG) && (CONFIG_BLE_OPUS_STREAM_RX_DEBUG)
    int cap = ring_queue_capacity(&s_player->queue);
    log_i("[BLE_OPUS] audio_dec_open: type=%s sr=%d ch=%d vol=%d prebuf=%d qsz=%d/%d",
          req.dec.dec_type,
          (int)req.dec.sample_rate,
          (int)req.dec.channel,
          (int)req.dec.volume,
          (int)CONFIG_BLE_OPUS_STREAM_PREBUF_BYTES,
          qsz,
          cap);
#endif
    int err = server_request(s_player->dec_server, AUDIO_REQ_DEC, &req);
    if (err) {
        log_e("[BLE_OPUS] audio_dec_open err=%d", err);
        return err;
    }

    s_player->cmp_file = (void *)&s_player->queue;
    s_player->dec_ops = &ble_opus_stream_dec_ops;

    req.dec.cmd = AUDIO_DEC_START;
    err = server_request(s_player->dec_server, AUDIO_REQ_DEC, &req);
    if (err) {
        log_e("[BLE_OPUS] audio_dec_start err=%d", err);
        return err;
    }

    s_playing = 1;
    log_i("[BLE_OPUS] start decode: sr=%d prebuf=%d qsz=%d",
          CONFIG_BLE_OPUS_STREAM_SAMPLE_RATE,
          (int)CONFIG_BLE_OPUS_STREAM_PREBUF_BYTES,
          qsz);
    return 0;
}

// -------------------- public API --------------------
void ble_opus_stream_rx_bind_player(music_player *player)
{
    s_player = player;
    log_i("[BLE_OPUS] bind player=%p dec_server=%p vol=%d", player,
          player ? player->dec_server : NULL,
          player ? player->volume : -1);
}

void ble_opus_stream_rx_on_connect(uint16_t conn_handle)
{
    (void)conn_handle;
    s_connected = 1;
    s_playing = 0;
    s_rx_bytes = 0;
    s_rx_pkts = 0;
    s_drop_full = 0;
    s_last_stat_ms = timer_get_ms();
    s_last_rx_ms = s_last_stat_ms;
    s_ignored_pkts = 0;
    s_ignored_bytes = 0;
    s_last_dec_status = -999;
    s_fread_calls = 0;
    s_fread_timeouts = 0;
    s_fread_disc_eos = 0;
    s_fread_err = 0;
    s_starve_spins = 0;
    if (s_player) {
        ring_queue_clear(&s_player->queue);
    }
    s_hdr_fill = 0;
    s_have_header = 0;
    s_framed = 0;
    s_expect_len = 0;
    s_got_len = 0;
    s_eof = 0;
    s_eof_reported = 0;
    log_i("[BLE_OPUS] connected");
}

void ble_opus_stream_rx_on_disconnect(uint16_t conn_handle)
{
    (void)conn_handle;
    s_connected = 0;
    log_i("[BLE_OPUS] disconnected (stop decode)");
    if (s_player) {
        ble_opus_dec_stop(s_player);
    }
    s_hdr_fill = 0;
    s_have_header = 0;
    s_framed = 0;
    s_expect_len = 0;
    s_got_len = 0;
    s_eof = 0;
    s_eof_reported = 0;
}

int ble_opus_stream_rx_on_audio_write(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) {
        return 0;
    }
    if (!s_connected || !s_player) {
        s_ignored_pkts++;
        s_ignored_bytes += (uint32_t)len;
#if defined(CONFIG_BLE_OPUS_STREAM_RX_DEBUG) && (CONFIG_BLE_OPUS_STREAM_RX_DEBUG)
        uint32_t now = timer_get_ms();
        if (s_last_stat_ms == 0 || (now - s_last_stat_ms) > 1000) {
            log_w("[BLE_OPUS] rx ignored: connected=%u player=%p pkts=%u bytes=%u",
                  s_connected, s_player, s_ignored_pkts, s_ignored_bytes);
        }
#endif
        return -1;
    }

    /*
     * Allow multiple "send -> play -> send again" sessions on the same BLE connection.
     *
     * In framed mode, after we reached EOF and the VFS reported EOS, we block auto-restart
     * (to avoid treating tail bytes as a new stream and producing noise).
     *
     * However, the *next* user-triggered upload will start with a new header. We must re-arm
     * header parsing for that new session. Otherwise, subsequent sends will be ignored and
     * you'll see "first send OK, second send silent".
     */
    if (s_framed && s_eof_reported) {
        log_i("[BLE_OPUS] new session: re-arm header after EOF");
        s_hdr_fill = 0;
        s_have_header = 0;
        s_framed = 0;
        s_expect_len = 0;
        s_got_len = 0;
        s_eof = 0;
        s_eof_reported = 0;
        if (s_player) {
            // Should already be empty after EOS, but keep it robust
            ring_queue_clear(&s_player->queue);
        }
    }

    // Decide framed vs legacy on first bytes
    const uint8_t *p = data;
    uint16_t nleft = len;

    if (!s_have_header) {
        uint16_t need = (uint16_t)(sizeof(ble_opus_hdr_t) - s_hdr_fill);
        uint16_t cpy = (nleft < need) ? nleft : need;
        memcpy(&s_hdr_buf[s_hdr_fill], p, cpy);
        s_hdr_fill += (uint8_t)cpy;
        p += cpy;
        nleft -= cpy;

        if (s_hdr_fill < sizeof(ble_opus_hdr_t)) {
            // not enough to decide yet
            return 0;
        }

        const ble_opus_hdr_t *hdr = (const ble_opus_hdr_t *)s_hdr_buf;
        if (hdr->magic_le == BLE_OPUS_HDR_MAGIC_LE) {
            // New framed session: reset decoder/queue and start receiving payload
            s_have_header = 1;
            s_framed = 1;
            s_expect_len = hdr->total_len_le;
            s_got_len = 0;
            s_eof = 0;
            s_eof_reported = 0;

            if (s_player) {
                // Ensure we don't play leftover bytes from previous session
                ble_opus_dec_stop(s_player);
                ring_queue_clear(&s_player->queue);
            }
            log_i("[BLE_OPUS] header: framed total_len=%u", (unsigned)s_expect_len);
        } else {
            // Legacy raw stream: treat header bytes as part of payload
            s_have_header = 1;
            s_framed = 0;
            s_expect_len = 0;
            s_got_len = 0;
            s_eof = 0;
            s_eof_reported = 0;

            int w0 = ring_queue_try_write(&s_player->queue, (const char *)s_hdr_buf, (int)sizeof(ble_opus_hdr_t));
            if (w0 < 0) {
                if (w0 == RINGQUEUE_Is_Full) {
                    s_drop_full++;
                }
                return -2;
            }
            s_rx_pkts++;
            s_rx_bytes += (uint32_t)w0;
            s_last_rx_ms = timer_get_ms();
            log_i("[BLE_OPUS] header: legacy stream (magic mismatch), pass-through %u bytes", (unsigned)sizeof(ble_opus_hdr_t));
        }
    }

    // Write payload bytes (framed with total length or legacy raw)
    while (nleft) {
        uint16_t take = nleft;
        if (s_framed && s_expect_len > 0) {
            if (s_got_len >= s_expect_len) {
                s_eof = 1;
                break; // ignore extra bytes until new connection/new header
            }
            uint32_t remain = s_expect_len - s_got_len;
            if (remain < take) {
                take = (uint16_t)remain;
            }
        }

        int w = ring_queue_try_write(&s_player->queue, (const char *)p, (int)take);
        if (w < 0) {
            if (w == RINGQUEUE_Is_Full) {
                s_drop_full++;
            }
            return -2;
        }

        p += (uint16_t)w;
        nleft -= (uint16_t)w;
        s_rx_pkts++;
        s_rx_bytes += (uint32_t)w;
        s_last_rx_ms = timer_get_ms();
        if (s_framed && s_expect_len > 0) {
            s_got_len += (uint32_t)w;
            if (s_got_len >= s_expect_len) {
                s_eof = 1;
                break;
            }
        }
    }

    // Account "stream RX" stats (kept for logs)
    if (s_framed && s_expect_len > 0) {
        // rx_bytes already includes written payload bytes; report eof progression via 1s log
    }

    uint32_t now = timer_get_ms();
    if (s_last_stat_ms == 0 || (now - s_last_stat_ms) > 1000) {
        int qsz = ring_queue_size(&s_player->queue);
        int cap = ring_queue_capacity(&s_player->queue);
        int dec_st = ble_opus_get_dec_status(s_player);
        if (dec_st != s_last_dec_status) {
            log_i("[BLE_OPUS] dec_status change: %d -> %d", s_last_dec_status, dec_st);
            s_last_dec_status = dec_st;
        }
        if (s_framed) {
            log_i("[BLE_OPUS] rx: pkts=%u bytes=%u drop_full=%u qsz=%d/%d prebuf=%d playing=%u dec_st=%d fread=%u timeout=%u disc_eos=%u err=%u framed=%u got=%u/%u eof=%u",
                  s_rx_pkts, s_rx_bytes, s_drop_full, qsz, cap,
                  (int)CONFIG_BLE_OPUS_STREAM_PREBUF_BYTES,
                  s_playing, dec_st,
                  s_fread_calls, s_fread_timeouts, s_fread_disc_eos, s_fread_err,
                  (unsigned)s_framed, (unsigned)s_got_len, (unsigned)s_expect_len, (unsigned)s_eof);
        } else {
            log_i("[BLE_OPUS] rx: pkts=%u bytes=%u drop_full=%u qsz=%d/%d prebuf=%d playing=%u dec_st=%d fread=%u timeout=%u disc_eos=%u err=%u",
              s_rx_pkts, s_rx_bytes, s_drop_full, qsz, cap,
              (int)CONFIG_BLE_OPUS_STREAM_PREBUF_BYTES,
              s_playing, dec_st,
              s_fread_calls, s_fread_timeouts, s_fread_disc_eos, s_fread_err);
        }
        s_last_stat_ms = now;
    }

    // lazy-start decode when enough data is buffered
    ble_opus_try_start_decode();
    return 0;
}

#endif /* CONFIG_BLE_OPUS_STREAM_RX */


