#include "marauder.h"

#include <furi.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <expansion/expansion.h>
#include <string.h>

#define MARAUDER_BAUD        115200
#define MARAUDER_LINE_MAX    256
#define MARAUDER_RX_BUF      1024
#define MARAUDER_GAP_MS      600 // let the board settle between passes
#define MARAUDER_GRACE_MS    5000 // silence beyond pass + grace = not responding
#define MARAUDER_POLL_MS     50
#define MARAUDER_INFO_MS     1500 // wait for `info` to report a version
#define MARAUDER_QUIET_MS    300 // `info` is done once the board goes quiet this long
#define MARAUDER_QUIET_MAX   2000 // ...but never wait longer than this
#define MARAUDER_PROBE_MS    3000 // wait for `scanall` to be acknowledged

// Marauder prints these; matched as substrings so wording tweaks survive
#define MARAUDER_VERSION_KEY "Version:"
#define MARAUDER_SCANALL_ACK "APs and Stations"

struct Marauder {
    FuriMutex* mutex;
    FuriThread* thread;
    FuriStreamBuffer* rx;
    FuriHalSerialHandle* serial;
    Expansion* expansion;

    // Config for the running walk
    MarauderMode mode;
    uint32_t pass_ms;
    MarauderLineCallback callback;
    void* context;
    volatile bool stop;
    volatile bool paused;

    // Line assembly (owner thread only)
    char line[MARAUDER_LINE_MAX];
    size_t line_len;
    bool line_overflow;
    bool forward; // pass lines to callback (off during the handshake)
    bool scanall_ack;
    char version[MARAUDER_VERSION_LEN];

    // Status (guarded by mutex)
    MarauderStatus status;
    uint32_t pass_start;
    uint32_t last_rx;
};

static void marauder_rx_isr(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* ctx) {
    Marauder* m = ctx;
    if(event == FuriHalSerialRxEventData) {
        uint8_t byte = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(m->rx, &byte, 1, 0);
    }
}

/** Wipe the receive buffer: it can hold raw addresses and network names. */
static void marauder_scrub_rx(Marauder* m) {
    static const uint8_t zeros[64] = {0};
    furi_stream_buffer_reset(m->rx);
    while(furi_stream_buffer_spaces_available(m->rx) > 0) {
        if(furi_stream_buffer_send(m->rx, zeros, sizeof(zeros), 0) == 0) break;
    }
    furi_stream_buffer_reset(m->rx);
}

/** Take the UART. Only one owner (walk worker or board check) at a time. */
static bool marauder_open(Marauder* m) {
    bool ok = false;
    furi_mutex_acquire(m->mutex, FuriWaitForever);
    if(!m->serial) {
        Expansion* expansion = furi_record_open(RECORD_EXPANSION);
        expansion_disable(expansion);
        FuriHalSerialHandle* handle = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
        if(handle) {
            m->expansion = expansion;
            m->serial = handle;
            furi_stream_buffer_reset(m->rx);
            furi_hal_serial_init(handle, MARAUDER_BAUD);
            furi_hal_serial_async_rx_start(handle, marauder_rx_isr, m, false);
            ok = true;
        } else {
            expansion_enable(expansion);
            furi_record_close(RECORD_EXPANSION);
        }
    }
    furi_mutex_release(m->mutex);
    return ok;
}

/** Release the UART. Only call after a successful marauder_open by the same caller. */
static void marauder_close(Marauder* m) {
    furi_mutex_acquire(m->mutex, FuriWaitForever);
    if(m->serial) {
        furi_hal_serial_async_rx_stop(m->serial);
        furi_hal_serial_deinit(m->serial);
        furi_hal_serial_control_release(m->serial);
        m->serial = NULL;
        marauder_scrub_rx(m);
    }
    if(m->expansion) {
        expansion_enable(m->expansion);
        furi_record_close(RECORD_EXPANSION);
        m->expansion = NULL;
    }
    furi_mutex_release(m->mutex);
}

static void marauder_send(Marauder* m, const char* cmd) {
    furi_hal_serial_tx(m->serial, (const uint8_t*)cmd, strlen(cmd));
    furi_hal_serial_tx(m->serial, (const uint8_t*)"\n", 1);
    furi_hal_serial_tx_wait_complete(m->serial);
}

/**
 * Empty Marauder's AP and station lists. Stations already in the list are
 * never printed again, so without this a second walk undercounts; it also
 * keeps network names from lingering in the board's memory after a walk.
 */
static void marauder_clear_board_lists(Marauder* m) {
    marauder_send(m, "clearlist -a");
    marauder_send(m, "clearlist -c");
}

/** Pick out handshake replies, then hand scan lines to the callback. */
static void marauder_handle_line(Marauder* m, const char* line) {
    if(!m->version[0]) {
        const char* v = strstr(line, MARAUDER_VERSION_KEY);
        if(v) {
            v += strlen(MARAUDER_VERSION_KEY);
            while(*v == ' ')
                v++;
            size_t n = 0;
            while(v[n] && v[n] != ' ' && n + 1 < sizeof(m->version)) {
                m->version[n] = v[n];
                n++;
            }
            m->version[n] = '\0';
        }
    }
    if(strstr(line, MARAUDER_SCANALL_ACK)) m->scanall_ack = true;
    if(m->forward && m->callback) m->callback(line, m->context);
}

/** Read whatever arrived and process complete lines. Returns bytes read. */
static size_t marauder_pump(Marauder* m, uint32_t timeout_ms) {
    uint8_t buf[64];
    size_t n = furi_stream_buffer_receive(m->rx, buf, sizeof(buf), timeout_ms);
    if(n == 0) return 0;

    furi_mutex_acquire(m->mutex, FuriWaitForever);
    m->last_rx = furi_get_tick();
    furi_mutex_release(m->mutex);

    for(size_t i = 0; i < n; i++) {
        char c = (char)buf[i];
        if(c == '\n' || c == '\r') {
            if(m->line_len > 0 && !m->line_overflow) {
                m->line[m->line_len] = '\0';
                marauder_handle_line(m, m->line);
                memset(m->line, 0, m->line_len);
            }
            m->line_len = 0;
            m->line_overflow = false;
        } else if(m->line_len + 1 < MARAUDER_LINE_MAX) {
            m->line[m->line_len++] = c;
        } else {
            m->line_overflow = true; // drop the rest of an over-long line
        }
    }
    // Lines may hold network names; don't leave them in the buffer
    memset(buf, 0, sizeof(buf));
    return n;
}

/** Pump until done() is true, timeout, or stop. Returns done(). */
static bool marauder_wait_for(Marauder* m, uint32_t timeout_ms, bool (*done)(Marauder*)) {
    uint32_t start = furi_get_tick();
    while(!m->stop && furi_get_tick() - start < timeout_ms) {
        if(done && done(m)) return true;
        marauder_pump(m, MARAUDER_POLL_MS);
    }
    return done && done(m);
}

/** Pump until the board has been silent for quiet_ms (or max_ms passes). */
static void marauder_wait_quiet(Marauder* m, uint32_t quiet_ms, uint32_t max_ms) {
    uint32_t start = furi_get_tick();
    uint32_t last = start;
    while(!m->stop && furi_get_tick() - start < max_ms && furi_get_tick() - last < quiet_ms) {
        if(marauder_pump(m, MARAUDER_POLL_MS)) last = furi_get_tick();
    }
}

static bool has_version(Marauder* m) {
    return m->version[0] != '\0';
}

static bool has_scanall_ack(Marauder* m) {
    return m->scanall_ack;
}

/**
 * Ask the board for its version and wait for `info` to finish: Marauder runs
 * one command at a time, so anything sent next is only answered after it.
 * `info` also leaves Marauder in its SHOW_INFO mode, and Marauder silently
 * ignores every scan command until the mode is off again, hence the stopscan.
 */
static void marauder_ask_info(Marauder* m, uint32_t timeout_ms) {
    marauder_send(m, "info");
    marauder_wait_for(m, timeout_ms, has_version);
    marauder_wait_quiet(m, MARAUDER_QUIET_MS, MARAUDER_QUIET_MAX);
    marauder_send(m, "stopscan");
    marauder_wait_quiet(m, MARAUDER_QUIET_MS, MARAUDER_QUIET_MAX);
}

/** Send scanall; true if acknowledged (the scan is then running). */
static bool marauder_probe_scanall(Marauder* m) {
    m->scanall_ack = false;
    marauder_send(m, "scanall");
    return marauder_wait_for(m, MARAUDER_PROBE_MS, has_scanall_ack);
}

static void marauder_reset_lines(Marauder* m) {
    m->line_len = 0;
    m->line_overflow = false;
    m->forward = false;
    m->scanall_ack = false;
    m->version[0] = '\0';
}

static void marauder_set_status(Marauder* m, MarauderPass pass, bool handshaking) {
    furi_mutex_acquire(m->mutex, FuriWaitForever);
    m->status.pass = pass;
    m->status.handshaking = handshaking;
    strlcpy(m->status.version, m->version, sizeof(m->status.version));
    m->pass_start = furi_get_tick();
    furi_mutex_release(m->mutex);
}

/** Run one pass of cmd for the configured duration, then stop and settle. */
static void marauder_run_pass(Marauder* m, const char* cmd, uint32_t already_ms) {
    if(cmd) marauder_send(m, cmd);
    uint32_t start = furi_get_tick() - already_ms;
    while(!m->stop && !m->paused && furi_get_tick() - start < m->pass_ms) {
        marauder_pump(m, MARAUDER_POLL_MS);
    }
    marauder_send(m, "stopscan");
    marauder_set_status(m, MarauderPassNone, false);
    marauder_wait_for(m, MARAUDER_GAP_MS, NULL);
}

/** If paused: stop the board and wait. On resume, clear its lists and carry on. */
static void marauder_hold_while_paused(Marauder* m) {
    if(!m->paused || m->stop) return;
    m->forward = false;
    marauder_send(m, "stopscan");
    marauder_set_status(m, MarauderPassNone, false);
    while(m->paused && !m->stop) {
        marauder_pump(m, MARAUDER_POLL_MS); // drain, dropping the lines
    }
    if(m->stop) return;
    marauder_clear_board_lists(m);
    marauder_wait_quiet(m, MARAUDER_QUIET_MS, MARAUDER_QUIET_MAX);
    furi_mutex_acquire(m->mutex, FuriWaitForever);
    m->last_rx = furi_get_tick(); // fresh grace period after the pause
    furi_mutex_release(m->mutex);
    m->forward = true;
}

static int32_t marauder_worker(void* ctx) {
    Marauder* m = ctx;
    marauder_reset_lines(m);

    if(!marauder_open(m)) {
        furi_mutex_acquire(m->mutex, FuriWaitForever);
        m->status.uart_busy = true;
        furi_mutex_release(m->mutex);
        while(!m->stop)
            furi_delay_ms(MARAUDER_POLL_MS);
        return 0;
    }

    // Handshake: quiet the board, empty its lists, ask its version
    marauder_set_status(m, MarauderPassNone, true);
    marauder_send(m, "stopscan");
    marauder_clear_board_lists(m);
    marauder_ask_info(m, MARAUDER_INFO_MS);
    m->forward = true;

    // In All mode, try scanall first; it counts as the first pass if acknowledged
    bool legacy = false;
    if(m->mode == MarauderModeAll && !m->stop) {
        marauder_set_status(m, MarauderPassAll, true);
        uint32_t probe_start = furi_get_tick();
        if(marauder_probe_scanall(m)) {
            marauder_set_status(m, MarauderPassAll, false);
            marauder_run_pass(m, NULL, furi_get_tick() - probe_start);
        } else {
            // Older Marauder: no scanall, use scansta + scanap
            legacy = true;
            marauder_send(m, "stopscan");
            marauder_set_status(m, MarauderPassNone, false);
            marauder_wait_for(m, MARAUDER_GAP_MS, NULL);
        }
    }

    bool next_sta = legacy;
    while(!m->stop) {
        marauder_hold_while_paused(m);
        if(m->stop) break;
        MarauderPass pass;
        const char* cmd;
        if(m->mode == MarauderModeAps) {
            pass = MarauderPassAp;
            cmd = "scanap";
        } else if(!legacy) {
            pass = MarauderPassAll;
            cmd = "scanall";
        } else if(next_sta) {
            pass = MarauderPassSta;
            cmd = "scansta";
        } else {
            pass = MarauderPassAp;
            cmd = "scanap";
        }
        next_sta = !next_sta;
        marauder_set_status(m, pass, false);
        marauder_run_pass(m, cmd, 0);
    }

    m->forward = false;
    marauder_send(m, "stopscan");
    marauder_clear_board_lists(m);
    marauder_close(m);
    memset(m->line, 0, sizeof(m->line));
    return 0;
}

Marauder* marauder_alloc(void) {
    Marauder* m = malloc(sizeof(Marauder));
    memset(m, 0, sizeof(Marauder));
    m->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    m->rx = furi_stream_buffer_alloc(MARAUDER_RX_BUF, 1);
    m->thread = furi_thread_alloc_ex("DoorstepUart", 2048, marauder_worker, m);
    return m;
}

void marauder_free(Marauder* m) {
    marauder_stop(m);
    furi_thread_free(m->thread);
    furi_stream_buffer_free(m->rx);
    furi_mutex_free(m->mutex);
    free(m);
}

void marauder_start(
    Marauder* m,
    MarauderMode mode,
    uint32_t pass_ms,
    MarauderLineCallback callback,
    void* context) {
    if(m->status.running) return;
    m->mode = mode;
    m->pass_ms = pass_ms;
    m->callback = callback;
    m->context = context;
    m->stop = false;
    m->paused = false;

    furi_mutex_acquire(m->mutex, FuriWaitForever);
    memset(&m->status, 0, sizeof(m->status));
    m->status.running = true;
    m->last_rx = furi_get_tick(); // grace period starts now
    m->pass_start = m->last_rx;
    furi_mutex_release(m->mutex);

    furi_thread_start(m->thread);
}

void marauder_set_paused(Marauder* m, bool paused) {
    m->paused = paused;
    furi_mutex_acquire(m->mutex, FuriWaitForever);
    m->status.paused = paused;
    furi_mutex_release(m->mutex);
}

void marauder_stop(Marauder* m) {
    if(!m->status.running) return;
    m->stop = true;
    furi_thread_join(m->thread);
    furi_mutex_acquire(m->mutex, FuriWaitForever);
    m->status.running = false;
    m->status.pass = MarauderPassNone;
    furi_mutex_release(m->mutex);
}

void marauder_get_status(Marauder* m, MarauderStatus* status) {
    furi_mutex_acquire(m->mutex, FuriWaitForever);
    *status = m->status;
    uint32_t now = furi_get_tick();
    if(m->status.pass != MarauderPassNone && m->pass_ms) {
        uint32_t elapsed = now - m->pass_start;
        status->progress = elapsed >= m->pass_ms ? 100 : (uint8_t)(elapsed * 100 / m->pass_ms);
    } else {
        status->progress = 0;
    }
    uint32_t grace = m->pass_ms + MARAUDER_GAP_MS + MARAUDER_GRACE_MS + MARAUDER_INFO_MS +
                     MARAUDER_QUIET_MAX + MARAUDER_PROBE_MS;
    status->responding = m->status.paused || (now - m->last_rx) < grace;
    furi_mutex_release(m->mutex);
}

bool marauder_check(
    Marauder* m,
    uint32_t timeout_ms,
    char* version,
    size_t version_len,
    bool* has_scanall) {
    if(version_len) version[0] = '\0';
    if(has_scanall) *has_scanall = false;
    if(m->status.running) return true;
    if(!marauder_open(m)) return false;

    marauder_reset_lines(m);
    m->callback = NULL;
    m->stop = false;
    uint32_t before = m->last_rx;
    marauder_ask_info(m, timeout_ms);
    bool heard = m->last_rx != before;
    if(version_len) strlcpy(version, m->version, version_len);

    if(heard) {
        // Same probe a walk uses, so Check board shows what a walk will do
        bool ack = marauder_probe_scanall(m);
        if(has_scanall) *has_scanall = ack;
        marauder_send(m, "stopscan");
        marauder_clear_board_lists(m);
        marauder_wait_quiet(m, MARAUDER_QUIET_MS, MARAUDER_QUIET_MAX);
    }

    marauder_close(m);
    memset(m->line, 0, sizeof(m->line));
    return heard;
}
