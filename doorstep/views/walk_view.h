#pragma once

#include <gui/view.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WalkAlertNone,
    WalkAlertNoBoard, // board not answering
    WalkAlertUartBusy, // UART held by another app/bridge
    WalkAlertMemoryFull, // device list full, no new devices counted
} WalkAlert;

typedef struct {
    uint32_t total;
    uint32_t elapsed_s;
    bool pass_active;
    const char* pass_label; // "ALL", "STA", "AP", "..." while handshaking; static string
    uint8_t progress; // 0-100
    bool paused;
    WalkAlert alert;
} WalkViewState;

typedef struct WalkView WalkView;
typedef void (*WalkViewBackCallback)(void* context);
typedef void (*WalkViewPauseCallback)(void* context);

WalkView* walk_view_alloc(void);
void walk_view_free(WalkView* walk_view);
View* walk_view_get_view(WalkView* walk_view);
void walk_view_set_back_callback(WalkView* walk_view, WalkViewBackCallback callback, void* context);

/** Called on a long press of OK (pause / resume). Uses the back callback's context. */
void walk_view_set_pause_callback(WalkView* walk_view, WalkViewPauseCallback callback);

/** Push new state; also advances the radio-icon pulse. Safe from any thread. */
void walk_view_update(WalkView* walk_view, const WalkViewState* state);
