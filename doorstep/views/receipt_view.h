#pragma once

#include <gui/view.h>
#include <stdbool.h>
#include <stdint.h>

#define RECEIPT_MAX_ROWS  34
#define RECEIPT_NAME_LEN  16
#define RECEIPT_TITLE_LEN 20

/** Pass as radios to leave the "Radios heard" row out. */
#define RECEIPT_RADIOS_UNKNOWN UINT32_MAX

typedef enum {
    ReceiptEventSave,
    ReceiptEventDone,
} ReceiptEvent;

/** Per-brand mark against your saved-walk average (per-hour rate). */
typedef enum {
    ReceiptTrendNone,
    ReceiptTrendUp,
    ReceiptTrendDown,
} ReceiptTrend;

typedef struct ReceiptView ReceiptView;
typedef void (*ReceiptViewCallback)(ReceiptEvent event, void* context);

ReceiptView* receipt_view_alloc(void);
void receipt_view_free(ReceiptView* receipt_view);
View* receipt_view_get_view(ReceiptView* receipt_view);
void receipt_view_set_callback(ReceiptView* receipt_view, ReceiptViewCallback callback, void* context);

/**
 * Clear rows and set the fixed lines: title, Duration, Radios heard, TOTAL.
 * radios_capped shows the radio count as "N+". Scrolls to top, fully revealed.
 */
void receipt_view_reset(
    ReceiptView* receipt_view,
    const char* title,
    uint32_t duration_s,
    uint32_t radios,
    bool radios_capped,
    uint32_t total);

void receipt_view_add_row(
    ReceiptView* receipt_view,
    const char* name,
    uint32_t value,
    ReceiptTrend trend);

/** Play the "printing" reveal: rows appear one by one, then TOTAL lands. Any key skips. */
void receipt_view_play_reveal(ReceiptView* receipt_view);
