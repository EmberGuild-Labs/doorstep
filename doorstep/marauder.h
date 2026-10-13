#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    MarauderModeAps, // access points only (scanap)
    MarauderModeAll, // APs and client stations
} MarauderMode;

typedef enum {
    MarauderPassNone,
    MarauderPassAp, // scanap
    MarauderPassSta, // scansta (Marauder before scanall)
    MarauderPassAll, // scanall
} MarauderPass;

#define MARAUDER_VERSION_LEN 16

typedef void (*MarauderLineCallback)(const char* line, void* context);

typedef struct {
    bool running;
    bool uart_busy; // UART held by something else (e.g. USB-UART bridge)
    bool handshaking; // asking the board for its version / probing scanall
    MarauderPass pass; // current pass, MarauderPassNone between passes
    uint8_t progress; // 0-100 through the current pass
    bool responding; // heard from the board recently (always true while paused)
    bool paused;
    char version[MARAUDER_VERSION_LEN]; // e.g. "v1.11.0", empty if unknown
} MarauderStatus;

typedef struct Marauder Marauder;

Marauder* marauder_alloc(void);
void marauder_free(Marauder* marauder);

/**
 * Start scanning on a worker thread. The worker first asks the board for its
 * version (`info`), then in All mode tries `scanall` and falls back to
 * alternating `scansta` / `scanap` on firmware that doesn't acknowledge it.
 * Scan lines go to callback on the worker thread.
 */
void marauder_start(
    Marauder* marauder,
    MarauderMode mode,
    uint32_t pass_ms,
    MarauderLineCallback callback,
    void* context);

/**
 * Pause or resume scanning. Paused: the board is stopped and lines are dropped.
 * On resume the board's lists are cleared so devices heard before the pause
 * are reported again (the walk's own dedupe keeps them from double counting).
 */
void marauder_set_paused(Marauder* marauder, bool paused);

/** Stop scanning, release the UART. Blocks until the worker exits. */
void marauder_stop(Marauder* marauder);

void marauder_get_status(Marauder* marauder, MarauderStatus* status);

/**
 * One-off check (not while scanning): true if the board answers within timeout.
 * version receives the Marauder version if reported, else an empty string.
 * has_scanall is set if the board acknowledged a `scanall` probe (as a walk would).
 */
bool marauder_check(
    Marauder* marauder,
    uint32_t timeout_ms,
    char* version,
    size_t version_len,
    bool* has_scanall);
