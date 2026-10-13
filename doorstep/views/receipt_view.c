#include "receipt_view.h"

#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <string.h>

struct ReceiptView {
    View* view;
    FuriTimer* reveal_timer;
    ReceiptViewCallback callback;
    void* context;
};

typedef enum {
    RowDuration,
    RowPerHour,
    RowRadios,
    RowBrand,
} RowKind;

typedef struct {
    RowKind kind;
    char name[RECEIPT_NAME_LEN];
    uint32_t value;
    ReceiptTrend trend;
} ReceiptRow;

typedef struct {
    char title[RECEIPT_TITLE_LEN];
    ReceiptRow rows[RECEIPT_MAX_ROWS + 3]; // + Duration, Cameras/hour, Radios heard
    uint8_t row_count;
    uint32_t duration_s;
    bool radios_capped;
    uint32_t total;
    bool has_trends;
    uint8_t scroll;
    // Reveal animation
    bool revealing;
    uint8_t revealed_rows; // visible rows printed so far
    uint8_t total_frame; // 0 = TOTAL not yet printed, then index into total_drop
} ReceiptViewModel;

// Layout (128x64)
#define HEADER_H      11
#define ROW_TOP       12
#define ROW_H         9
#define VISIBLE_ROWS  4
#define VALUE_X       120 // right edge of numbers
#define VALUE_X_TREND 114 // when trend marks take the column to the right
#define TREND_X       117
#define SCROLL_X      127
#define TOTAL_Y       49
#define TOTAL_H       10
#define TEAR_Y        (TOTAL_Y + TOTAL_H)

// Reveal: 4 rows x 40 ms, then TOTAL rises into place over 3 frames,
// decelerating (ease-out). About 280 ms in all.
#define REVEAL_TICK_MS 40
static const uint8_t total_drop[] = {0, 6, 2, 0}; // [0] unused: not drawn yet
#define TOTAL_FRAMES (sizeof(total_drop) - 1)

static void draw_leaders(Canvas* canvas, int32_t from_x, int32_t to_x, int32_t y) {
    // Start on a multiple of 3 so dots line up in a column across rows
    for(int32_t x = from_x + (3 - from_x % 3) % 3; x <= to_x; x += 3) {
        canvas_draw_dot(canvas, x, y);
    }
}

static void draw_row(
    Canvas* canvas,
    int32_t baseline,
    int32_t value_x,
    const char* name,
    const char* value,
    bool leaders) {
    canvas_draw_str(canvas, 2, baseline, name);
    canvas_draw_str_aligned(canvas, value_x, baseline, AlignRight, AlignBottom, value);
    if(leaders) {
        int32_t name_end = 2 + canvas_string_width(canvas, name) + 3;
        int32_t value_start = value_x - canvas_string_width(canvas, value) - 3;
        draw_leaders(canvas, name_end, value_start, baseline - 1);
    }
}

static void draw_trend(Canvas* canvas, int32_t baseline, ReceiptTrend trend) {
    // 5x3 triangle, vertically centred on the text
    int32_t top = baseline - 5;
    if(trend == ReceiptTrendUp) {
        canvas_draw_dot(canvas, TREND_X + 2, top);
        canvas_draw_line(canvas, TREND_X + 1, top + 1, TREND_X + 3, top + 1);
        canvas_draw_line(canvas, TREND_X, top + 2, TREND_X + 4, top + 2);
    } else if(trend == ReceiptTrendDown) {
        canvas_draw_line(canvas, TREND_X, top, TREND_X + 4, top);
        canvas_draw_line(canvas, TREND_X + 1, top + 1, TREND_X + 3, top + 1);
        canvas_draw_dot(canvas, TREND_X + 2, top + 2);
    }
}

static void draw_torn_edge(Canvas* canvas, int32_t y) {
    // Teeth hanging off the TOTAL bar, like a receipt torn off the roll
    for(int32_t x = 0; x < 128; x++) {
        int32_t d = x % 6;
        int32_t h = d < 3 ? d : 6 - d;
        canvas_draw_line(canvas, x, y, x, y + 3 - h);
    }
}

static void format_duration(char* buf, size_t len, uint32_t t) {
    snprintf(
        buf,
        len,
        "%02lu:%02lu:%02lu",
        (unsigned long)(t / 3600),
        (unsigned long)((t / 60) % 60),
        (unsigned long)(t % 60));
}

static void receipt_view_draw(Canvas* canvas, void* model_ptr) {
    ReceiptViewModel* model = model_ptr;
    char buf[16];
    int32_t value_x = model->has_trends ? VALUE_X_TREND : VALUE_X;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    // Header bar
    canvas_draw_box(canvas, 0, 0, 128, HEADER_H);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, HEADER_H - 1, AlignCenter, AlignBottom, model->title);
    canvas_set_color(canvas, ColorBlack);

    // Scrolling rows
    canvas_set_font(canvas, FontSecondary);
    uint8_t shown = model->revealing ? model->revealed_rows : VISIBLE_ROWS;
    for(uint8_t i = 0; i < VISIBLE_ROWS && i < shown; i++) {
        uint8_t idx = model->scroll + i;
        if(idx >= model->row_count) break;
        const ReceiptRow* row = &model->rows[idx];
        int32_t baseline = ROW_TOP + (i + 1) * ROW_H - 1;
        switch(row->kind) {
        case RowDuration:
            // Duration has no leaders: it isn't a count
            format_duration(buf, sizeof(buf), model->duration_s);
            draw_row(canvas, baseline, value_x, "Duration", buf, false);
            break;
        case RowPerHour:
            // Cameras per hour; too noisy to mean anything under a minute
            if(model->duration_s < 60) {
                snprintf(buf, sizeof(buf), "-");
            } else {
                uint32_t tenths = (uint32_t)((uint64_t)model->total * 36000 / model->duration_s);
                snprintf(
                    buf, sizeof(buf), "%lu.%lu", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
            }
            draw_row(canvas, baseline, value_x, "Cameras/hour", buf, false);
            break;
        case RowRadios:
            snprintf(
                buf,
                sizeof(buf),
                "%lu%s",
                (unsigned long)row->value,
                model->radios_capped ? "+" : "");
            draw_row(canvas, baseline, value_x, "Radios heard", buf, false);
            break;
        case RowBrand:
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)row->value);
            draw_row(canvas, baseline, value_x, row->name, buf, true);
            draw_trend(canvas, baseline, row->trend);
            break;
        }
    }

    if(model->row_count > VISIBLE_ROWS && !model->revealing) {
        elements_scrollbar_pos(
            canvas,
            SCROLL_X,
            ROW_TOP,
            VISIBLE_ROWS * ROW_H,
            model->scroll,
            model->row_count - VISIBLE_ROWS + 1);
    }

    // Pinned TOTAL bar, printed last during the reveal
    if(model->revealing && model->total_frame == 0) return;
    int32_t drop = model->revealing ? total_drop[model->total_frame] : 0;
    int32_t bar_y = TOTAL_Y + drop;
    canvas_draw_box(canvas, 0, bar_y, 128, TOTAL_H);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    int32_t total_base = bar_y + TOTAL_H - 1;
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)model->total);
    canvas_draw_str(canvas, 2, total_base, "TOTAL");
    canvas_draw_str_aligned(canvas, value_x, total_base, AlignRight, AlignBottom, buf);
    draw_leaders(
        canvas,
        2 + canvas_string_width(canvas, "TOTAL") + 3,
        value_x - canvas_string_width(canvas, buf) - 3,
        total_base - 1);
    canvas_set_color(canvas, ColorBlack);

    draw_torn_edge(canvas, bar_y + TOTAL_H);
}

static void reveal_finish(ReceiptView* receipt_view) {
    furi_timer_stop(receipt_view->reveal_timer);
    with_view_model(
        receipt_view->view, ReceiptViewModel * model, { model->revealing = false; }, true);
}

static void reveal_tick(void* context) {
    // Timer thread. Never stop the timer from here (it would block the timer
    // daemon on its own queue); it is stopped from the GUI thread on finish,
    // reset or view exit, and idles as a no-op until then.
    ReceiptView* receipt_view = context;
    bool revealing = false;
    with_view_model(
        receipt_view->view, ReceiptViewModel * model, { revealing = model->revealing; }, false);
    if(!revealing) return;

    with_view_model(
        receipt_view->view,
        ReceiptViewModel * model,
        {
            uint8_t rows = model->row_count < VISIBLE_ROWS ? model->row_count : VISIBLE_ROWS;
            if(model->revealed_rows < rows) {
                model->revealed_rows++;
            } else if(model->total_frame < TOTAL_FRAMES) {
                model->total_frame++;
            } else {
                model->revealing = false;
            }
        },
        true);
}

static void receipt_view_exit(void* context) {
    ReceiptView* receipt_view = context;
    furi_timer_stop(receipt_view->reveal_timer);
}

static bool receipt_view_input(InputEvent* event, void* context) {
    ReceiptView* receipt_view = context;

    // Any key during the reveal just finishes it
    bool revealing = false;
    with_view_model(
        receipt_view->view, ReceiptViewModel * model, { revealing = model->revealing; }, false);
    if(revealing) {
        if(event->type == InputTypeShort) reveal_finish(receipt_view);
        return true;
    }

    if(event->type != InputTypeShort && event->type != InputTypeRepeat) {
        return event->key == InputKeyBack;
    }

    switch(event->key) {
    case InputKeyUp:
    case InputKeyDown: {
        bool down = event->key == InputKeyDown;
        with_view_model(
            receipt_view->view,
            ReceiptViewModel * model,
            {
                uint8_t max = model->row_count > VISIBLE_ROWS ? model->row_count - VISIBLE_ROWS :
                                                                 0;
                if(down && model->scroll < max) model->scroll++;
                if(!down && model->scroll > 0) model->scroll--;
            },
            true);
        return true;
    }
    case InputKeyOk:
        if(event->type == InputTypeShort && receipt_view->callback) {
            receipt_view->callback(ReceiptEventSave, receipt_view->context);
        }
        return true;
    case InputKeyBack:
        if(event->type == InputTypeShort && receipt_view->callback) {
            receipt_view->callback(ReceiptEventDone, receipt_view->context);
        }
        return true;
    default:
        return false;
    }
}

ReceiptView* receipt_view_alloc(void) {
    ReceiptView* receipt_view = malloc(sizeof(ReceiptView));
    receipt_view->callback = NULL;
    receipt_view->context = NULL;
    receipt_view->view = view_alloc();
    view_allocate_model(receipt_view->view, ViewModelTypeLocking, sizeof(ReceiptViewModel));
    view_set_context(receipt_view->view, receipt_view);
    view_set_draw_callback(receipt_view->view, receipt_view_draw);
    view_set_input_callback(receipt_view->view, receipt_view_input);
    view_set_exit_callback(receipt_view->view, receipt_view_exit);
    receipt_view->reveal_timer =
        furi_timer_alloc(reveal_tick, FuriTimerTypePeriodic, receipt_view);
    return receipt_view;
}

void receipt_view_free(ReceiptView* receipt_view) {
    furi_timer_stop(receipt_view->reveal_timer);
    furi_timer_free(receipt_view->reveal_timer);
    view_free(receipt_view->view);
    free(receipt_view);
}

View* receipt_view_get_view(ReceiptView* receipt_view) {
    return receipt_view->view;
}

void receipt_view_set_callback(ReceiptView* receipt_view, ReceiptViewCallback callback, void* context) {
    receipt_view->callback = callback;
    receipt_view->context = context;
}

void receipt_view_reset(
    ReceiptView* receipt_view,
    const char* title,
    uint32_t duration_s,
    uint32_t radios,
    bool radios_capped,
    uint32_t total) {
    furi_timer_stop(receipt_view->reveal_timer);
    with_view_model(
        receipt_view->view,
        ReceiptViewModel * model,
        {
            memset(model, 0, sizeof(ReceiptViewModel));
            strlcpy(model->title, title, sizeof(model->title));
            model->duration_s = duration_s;
            model->radios_capped = radios_capped;
            model->total = total;
            model->rows[0].kind = RowDuration;
            model->rows[1].kind = RowPerHour;
            model->row_count = 2;
            if(radios != RECEIPT_RADIOS_UNKNOWN) {
                model->rows[2].kind = RowRadios;
                model->rows[2].value = radios;
                model->row_count = 3;
            }
        },
        true);
}

void receipt_view_add_row(
    ReceiptView* receipt_view,
    const char* name,
    uint32_t value,
    ReceiptTrend trend) {
    with_view_model(
        receipt_view->view,
        ReceiptViewModel * model,
        {
            if(model->row_count < COUNT_OF(model->rows)) {
                ReceiptRow* row = &model->rows[model->row_count++];
                row->kind = RowBrand;
                strlcpy(row->name, name, sizeof(row->name));
                row->value = value;
                row->trend = trend;
                if(trend != ReceiptTrendNone) model->has_trends = true;
            }
        },
        true);
}

void receipt_view_play_reveal(ReceiptView* receipt_view) {
    with_view_model(
        receipt_view->view,
        ReceiptViewModel * model,
        {
            model->scroll = 0;
            model->revealing = true;
            model->revealed_rows = 0;
            model->total_frame = 0;
        },
        true);
    furi_timer_start(receipt_view->reveal_timer, furi_ms_to_ticks(REVEAL_TICK_MS));
}
