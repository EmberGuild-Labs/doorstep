#include "walk_view.h"

#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>

struct WalkView {
    View* view;
    WalkViewBackCallback back_callback;
    WalkViewPauseCallback pause_callback;
    void* back_context;
};

typedef struct {
    WalkViewState state;
    uint8_t pulse;
} WalkViewModel;

// Layout (128x64)
#define TOP_BASELINE  9
#define RULE_Y        11
#define RADIO_X       76
#define RADIO_Y       5
#define NUMBER_Y      37 // bottom of big digits
#define LABEL_Y       47
#define STRIP_Y       52 // bottom strip: progress bar or alert
#define BAR_X         2
#define BAR_Y         55
#define BAR_W         96
#define BAR_H         7

static void draw_radio(Canvas* canvas, bool active, uint8_t pulse) {
    canvas_draw_disc(canvas, RADIO_X, RADIO_Y, 1);
    if(!active) {
        canvas_draw_circle(canvas, RADIO_X, RADIO_Y, 3);
        return;
    }
    // Rings grow outward while a pass is running
    uint8_t phase = pulse % 3;
    if(phase >= 1) canvas_draw_circle(canvas, RADIO_X, RADIO_Y, 3);
    if(phase >= 2) canvas_draw_circle(canvas, RADIO_X, RADIO_Y, 5);
}

static const char* strip_alert(const WalkViewState* s) {
    if(s->paused) return "PAUSED - hold OK";
    switch(s->alert) {
    case WalkAlertNoBoard:
        return "! Board not responding";
    case WalkAlertUartBusy:
        return "! UART in use";
    case WalkAlertMemoryFull:
        return "! Memory full";
    default:
        return NULL;
    }
}

static void draw_strip(Canvas* canvas, const WalkViewState* s) {
    const char* alert = strip_alert(s);

    if(alert) {
        canvas_draw_box(canvas, 0, STRIP_Y, 128, 64 - STRIP_Y);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignCenter, alert);
        canvas_set_color(canvas, ColorBlack);
        return;
    }

    canvas_draw_frame(canvas, BAR_X, BAR_Y, BAR_W, BAR_H);
    uint8_t fill = (uint8_t)((BAR_W - 4) * s->progress / 100);
    if(fill) canvas_draw_box(canvas, BAR_X + 2, BAR_Y + 2, fill, BAR_H - 4);

    canvas_draw_str_aligned(
        canvas, 126, BAR_Y + BAR_H, AlignRight, AlignBottom, s->pass_label ? s->pass_label : "");
}

static void walk_view_draw(Canvas* canvas, void* model_ptr) {
    WalkViewModel* model = model_ptr;
    const WalkViewState* s = &model->state;
    char buf[16];

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    // Top bar
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, TOP_BASELINE, "DOORSTEP");
    draw_radio(canvas, s->pass_active && !s->paused, model->pulse);
    uint32_t t = s->elapsed_s;
    snprintf(
        buf,
        sizeof(buf),
        "%02lu:%02lu:%02lu",
        (unsigned long)(t / 3600),
        (unsigned long)((t / 60) % 60),
        (unsigned long)(t % 60));
    canvas_draw_str_aligned(canvas, 126, TOP_BASELINE, AlignRight, AlignBottom, buf);
    canvas_draw_line(canvas, 0, RULE_Y, 127, RULE_Y);

    // Running total
    canvas_set_font(canvas, FontBigNumbers);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)s->total);
    canvas_draw_str_aligned(canvas, 64, NUMBER_Y, AlignCenter, AlignBottom, buf);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas,
        64,
        LABEL_Y,
        AlignCenter,
        AlignBottom,
        s->total == 1 ? "camera passed" : "cameras passed");

    draw_strip(canvas, s);
}

static bool walk_view_input(InputEvent* event, void* context) {
    WalkView* walk_view = context;
    if(event->key == InputKeyBack && event->type == InputTypeShort) {
        if(walk_view->back_callback) walk_view->back_callback(walk_view->back_context);
        return true;
    }
    // Long press only, so a pocket bump can't pause the walk
    if(event->key == InputKeyOk && event->type == InputTypeLong) {
        if(walk_view->pause_callback) walk_view->pause_callback(walk_view->back_context);
        return true;
    }
    // Swallow everything else so a pocket press can't leave the walk
    return event->key == InputKeyBack;
}

WalkView* walk_view_alloc(void) {
    WalkView* walk_view = malloc(sizeof(WalkView));
    walk_view->back_callback = NULL;
    walk_view->pause_callback = NULL;
    walk_view->back_context = NULL;
    walk_view->view = view_alloc();
    view_allocate_model(walk_view->view, ViewModelTypeLocking, sizeof(WalkViewModel));
    view_set_context(walk_view->view, walk_view);
    view_set_draw_callback(walk_view->view, walk_view_draw);
    view_set_input_callback(walk_view->view, walk_view_input);
    return walk_view;
}

void walk_view_free(WalkView* walk_view) {
    view_free(walk_view->view);
    free(walk_view);
}

void walk_view_set_pause_callback(WalkView* walk_view, WalkViewPauseCallback callback) {
    walk_view->pause_callback = callback;
}

View* walk_view_get_view(WalkView* walk_view) {
    return walk_view->view;
}

void walk_view_set_back_callback(WalkView* walk_view, WalkViewBackCallback callback, void* context) {
    walk_view->back_callback = callback;
    walk_view->back_context = context;
}

void walk_view_update(WalkView* walk_view, const WalkViewState* state) {
    with_view_model(
        walk_view->view,
        WalkViewModel * model,
        {
            model->state = *state;
            model->pulse++;
        },
        true);
}
