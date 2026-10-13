// Doorstep: tally which camera brands were within Wi-Fi range on a walk.
// Counts per brand plus walk duration only. See README.md for scope.

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/popup.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/text_box.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#include "rules.h"
#include "tally.h"
#include "marauder.h"
#include "history.h"
#include "views/walk_view.h"
#include "views/receipt_view.h"

#define DOORSTEP_VERSION "1.1"

#define RULES_PATH    APP_DATA_PATH("rules.txt")
#define RULES_ASSET   APP_ASSETS_PATH("rules.txt")
#define TALLIES_PATH  APP_DATA_PATH("tallies.txt")
#define SETTINGS_PATH APP_DATA_PATH("settings.txt")

#define SETTINGS_FILETYPE "Doorstep Settings"
#define SETTINGS_VERSION  2 // v2: Mode is APs/All

#define UI_TICK_MS      250
#define BUZZ_GAP_MS     5000 // at most one "new camera" buzz per 5 s
#define LOW_BATTERY_PCT 10 // end and save the walk below this (unless charging)
#define LOW_BATTERY_MIN 3 // ...or this, if the walk started below LOW_BATTERY_PCT
#define LIFETIME_INDEX  0x10000 // Saved Walks item for lifetime totals
#define CHECK_TIMEOUT   2000

#define SAVED_LIST_MAX  50 // newest walks shown in Saved Walks
#define TREND_BAND      0.15f // +/-15% of your average per-hour rate

typedef enum {
    ViewMenu,
    ViewWalk,
    ViewReceipt,
    ViewDialog,
    ViewPopup,
    ViewSettings,
    ViewAbout,
    ViewSaved,
} DoorstepView;

typedef enum {
    MenuStartWalk,
    MenuSaved,
    MenuRules,
    MenuSettings,
    MenuAbout,
} MenuIndex;

typedef enum {
    DialogEndWalk,
    DialogSave,
    DialogDiscard,
    DialogRules,
    DialogDeleteWalk,
} DialogKind;

typedef enum {
    EventCheckOk = 100,
    EventCheckFail,
    EventLowBattery,
    // Popup timeouts: EventPopupDone + (generation & 0xFFFF)
    EventPopupDone = 0x10000,
} DoorstepEvent;

// Settings option tables
static const uint32_t pass_values_s[] = {10, 20, 30, 60};
static const char* const pass_labels[] = {"10 s", "20 s", "30 s", "60 s"};
static const char* const mode_labels[] = {"APs", "All"}; // MarauderMode order
static const int16_t range_rssi[] = {-65, -75, TALLY_RSSI_ANY};
static const char* const range_labels[] = {"Near", "Street", "Max"};
static const char* const buzz_labels[] = {"Off", "On"};

// Memory full: three long buzzes with the red LED, once per walk
static const NotificationSequence sequence_memory_full = {
    &message_red_255,
    &message_vibro_on,
    &message_delay_250,
    &message_vibro_off,
    &message_delay_100,
    &message_vibro_on,
    &message_delay_250,
    &message_vibro_off,
    &message_delay_100,
    &message_vibro_on,
    &message_delay_250,
    &message_vibro_off,
    &message_red_0,
    NULL,
};

typedef struct {
    uint8_t pass; // index into pass_values_s
    uint8_t mode; // MarauderMode
    uint8_t range; // index into range_rssi
    uint8_t buzz; // 0/1
} Settings;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    NotificationApp* notifications;

    Submenu* menu;
    WalkView* walk_view;
    ReceiptView* receipt_view;
    DialogEx* dialog;
    Popup* popup;
    VariableItemList* settings_list;
    TextBox* about;
    Submenu* saved_menu;

    DoorstepView current_view;
    DialogKind dialog_kind;
    DoorstepView popup_return;
    uint16_t popup_generation;
    bool checking; // board check owns the popup until it reports

    Rules* rules;
    RulesLoadResult rules_result;
    Tally* tally;
    FuriMutex* tally_mutex;
    Marauder* marauder;
    FuriTimer* ui_timer;
    FuriThread* check_thread;

    Settings settings;
    uint32_t walk_start;
    uint32_t paused_ticks; // total time spent paused, excluded from duration
    uint32_t pause_started;
    bool paused;
    uint32_t last_buzz;
    bool buzzed_once;
    uint32_t walk_duration_s;
    bool receipt_saved;
    bool receipt_is_history; // showing a saved walk, not a fresh one
    uint32_t history_index; // which saved walk (or LIFETIME_INDEX)
    bool walking;
    bool full_alerted;
    bool low_battery_sent;
    uint8_t low_battery_pct;
    char rules_text[64];
    char check_version[MARAUDER_VERSION_LEN];
    bool check_scanall;
} Doorstep;

static void show_view(Doorstep* app, DoorstepView view) {
    app->current_view = view;
    view_dispatcher_switch_to_view(app->view_dispatcher, view);
}

// ---------------------------------------------------------------- settings

static void settings_defaults(Settings* s) {
    s->pass = 1; // 20 s
    s->mode = MarauderModeAll;
    s->range = 1; // Street
    s->buzz = 0;
}

static void settings_load(Doorstep* app) {
    settings_defaults(&app->settings);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    FuriString* filetype = furi_string_alloc();
    uint32_t version = 0, v = 0;

    if(flipper_format_file_open_existing(ff, SETTINGS_PATH) &&
       flipper_format_read_header(ff, filetype, &version) &&
       furi_string_equal_str(filetype, SETTINGS_FILETYPE) && version == SETTINGS_VERSION) {
        if(flipper_format_read_uint32(ff, "Pass", &v, 1) && v < COUNT_OF(pass_values_s))
            app->settings.pass = v;
        if(flipper_format_read_uint32(ff, "Mode", &v, 1) && v < COUNT_OF(mode_labels))
            app->settings.mode = v;
        if(flipper_format_read_uint32(ff, "Range", &v, 1) && v < COUNT_OF(range_rssi))
            app->settings.range = v;
        if(flipper_format_read_uint32(ff, "Buzz", &v, 1) && v < 2) app->settings.buzz = v;
    }

    furi_string_free(filetype);
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
}

static void settings_save(Doorstep* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    uint32_t v;

    if(flipper_format_file_open_always(ff, SETTINGS_PATH) &&
       flipper_format_write_header_cstr(ff, SETTINGS_FILETYPE, SETTINGS_VERSION)) {
        v = app->settings.pass;
        flipper_format_write_uint32(ff, "Pass", &v, 1);
        v = app->settings.mode;
        flipper_format_write_uint32(ff, "Mode", &v, 1);
        v = app->settings.range;
        flipper_format_write_uint32(ff, "Range", &v, 1);
        v = app->settings.buzz;
        flipper_format_write_uint32(ff, "Buzz", &v, 1);
    }

    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
}

// ---------------------------------------------------------------- rules

static void rules_install_default(void) {
    // First run: copy the bundled rules to the SD card so they can be edited
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage_file_exists(storage, RULES_PATH) && storage_file_exists(storage, RULES_ASSET)) {
        storage_common_copy(storage, RULES_ASSET, RULES_PATH);
    }
    furi_record_close(RECORD_STORAGE);
}

static void rules_reload(Doorstep* app) {
    app->rules_result = rules_load(app->rules, RULES_PATH);
    switch(app->rules_result) {
    case RulesLoadOk:
        snprintf(
            app->rules_text,
            sizeof(app->rules_text),
            "%u brands\n%u prefixes\n%u name patterns",
            app->rules->brand_count,
            app->rules->prefix_count,
            app->rules->name_count);
        break;
    case RulesLoadEmpty:
        snprintf(app->rules_text, sizeof(app->rules_text), "rules.txt has\nno brands");
        break;
    default:
        snprintf(
            app->rules_text, sizeof(app->rules_text), "No rules.txt in\napps_data/doorstep/");
        break;
    }
}

// ---------------------------------------------------------------- popup / dialog helpers

static void popup_done_callback(void* context) {
    // Runs on the timer thread: hand off to the GUI thread instead of switching here
    Doorstep* app = context;
    view_dispatcher_send_custom_event(
        app->view_dispatcher, EventPopupDone + app->popup_generation);
}

static void show_popup(
    Doorstep* app,
    const char* header,
    const char* text,
    uint32_t timeout_ms,
    DoorstepView return_to) {
    popup_reset(app->popup);
    popup_set_header(app->popup, header, 64, 20, AlignCenter, AlignBottom);
    if(text) popup_set_text(app->popup, text, 64, 26, AlignCenter, AlignTop);
    popup_set_context(app->popup, app);
    app->popup_return = return_to;
    app->popup_generation++;
    if(timeout_ms) {
        popup_set_callback(app->popup, popup_done_callback);
        popup_set_timeout(app->popup, timeout_ms);
        popup_enable_timeout(app->popup);
    }
    show_view(app, ViewPopup);
}

static void dialog_callback(DialogExResult result, void* context);

static void show_dialog(
    Doorstep* app,
    DialogKind kind,
    const char* header,
    const char* text,
    const char* left,
    const char* center,
    const char* right) {
    app->dialog_kind = kind;
    // dialog_ex_reset clears the result callback too, so set it again every time
    dialog_ex_reset(app->dialog);
    dialog_ex_set_result_callback(app->dialog, dialog_callback);
    dialog_ex_set_context(app->dialog, app);
    dialog_ex_set_header(app->dialog, header, 64, 2, AlignCenter, AlignTop);
    dialog_ex_set_text(app->dialog, text, 64, 30, AlignCenter, AlignCenter);
    if(left) dialog_ex_set_left_button_text(app->dialog, left);
    if(center) dialog_ex_set_center_button_text(app->dialog, center);
    if(right) dialog_ex_set_right_button_text(app->dialog, right);
    show_view(app, ViewDialog);
}

// ---------------------------------------------------------------- walk

static void walk_line_callback(const char* line, void* context) {
    // Worker thread
    Doorstep* app = context;
    furi_mutex_acquire(app->tally_mutex, FuriWaitForever);
    uint8_t added =
        tally_feed_line(app->tally, app->rules, line, range_rssi[app->settings.range]);
    furi_mutex_release(app->tally_mutex);

    // Rate-limited: a burst of cameras gives one buzz, not a buzz each
    uint32_t now = furi_get_tick();
    if(added && app->settings.buzz &&
       (!app->buzzed_once || now - app->last_buzz >= furi_ms_to_ticks(BUZZ_GAP_MS))) {
        app->buzzed_once = true;
        app->last_buzz = now;
        notification_message(app->notifications, &sequence_single_vibro);
        notification_message(app->notifications, &sequence_blink_green_10);
    }
}

/** Walk time so far in seconds, leaving out time spent paused. */
static uint32_t walk_elapsed_s(Doorstep* app) {
    uint32_t now = furi_get_tick();
    uint32_t paused = app->paused_ticks + (app->paused ? now - app->pause_started : 0);
    return (now - app->walk_start - paused) / furi_kernel_get_tick_frequency();
}

static void ui_timer_callback(void* context) {
    Doorstep* app = context;
    MarauderStatus status;
    marauder_get_status(app->marauder, &status);

    WalkViewState state = {0};
    furi_mutex_acquire(app->tally_mutex, FuriWaitForever);
    state.total = app->tally->total;
    bool full = app->tally->full;
    furi_mutex_release(app->tally_mutex);

    state.elapsed_s = walk_elapsed_s(app);
    state.paused = app->paused;
    state.pass_active = status.pass != MarauderPassNone && !status.handshaking;
    switch(status.pass) {
    case MarauderPassAll:
        state.pass_label = "ALL";
        break;
    case MarauderPassSta:
        state.pass_label = "STA";
        break;
    case MarauderPassAp:
        state.pass_label = "AP";
        break;
    default:
        state.pass_label = "";
        break;
    }
    if(status.handshaking) state.pass_label = "...";
    state.progress = status.progress;
    if(status.uart_busy)
        state.alert = WalkAlertUartBusy;
    else if(!status.responding)
        state.alert = WalkAlertNoBoard;
    else if(full)
        state.alert = WalkAlertMemoryFull;

    walk_view_update(app->walk_view, &state);

    // Tell the pocket once that no more cameras can be counted
    if(full && !app->full_alerted) {
        app->full_alerted = true;
        notification_message(app->notifications, &sequence_memory_full);
    }

    // Low battery: end and save on the GUI thread before the Flipper dies
    if(!app->low_battery_sent && !furi_hal_power_is_charging() &&
       furi_hal_power_get_pct() < app->low_battery_pct) {
        app->low_battery_sent = true;
        view_dispatcher_send_custom_event(app->view_dispatcher, EventLowBattery);
    }
}

static void walk_start(Doorstep* app) {
    if(app->checking) return; // board check still owns the UART
    if(app->rules_result != RulesLoadOk) {
        show_popup(app, "No rules loaded", app->rules_text, 2500, ViewMenu);
        return;
    }

    furi_mutex_acquire(app->tally_mutex, FuriWaitForever);
    tally_reset(app->tally);
    furi_mutex_release(app->tally_mutex);

    app->walk_start = furi_get_tick();
    app->paused = false;
    app->paused_ticks = 0;
    app->buzzed_once = false;
    app->full_alerted = false;
    app->low_battery_sent = false;
    // Starting already low: only step in when it's nearly empty
    app->low_battery_pct =
        furi_hal_power_get_pct() < LOW_BATTERY_PCT ? LOW_BATTERY_MIN : LOW_BATTERY_PCT;
    app->walking = true;
    marauder_start(
        app->marauder,
        (MarauderMode)app->settings.mode,
        pass_values_s[app->settings.pass] * 1000,
        walk_line_callback,
        app);

    ui_timer_callback(app);
    furi_timer_start(app->ui_timer, furi_ms_to_ticks(UI_TICK_MS));
    show_view(app, ViewWalk);
}

static void walk_stop(Doorstep* app, bool reveal) {
    furi_timer_stop(app->ui_timer);
    app->walking = false;
    app->walk_duration_s = walk_elapsed_s(app);
    marauder_stop(app->marauder);
    app->paused = false;

    furi_mutex_acquire(app->tally_mutex, FuriWaitForever);
    // Discard the device hashes and salt now; only counts survive
    tally_forget_devices(app->tally);

    receipt_view_reset(
        app->receipt_view,
        "WALK RECEIPT",
        app->walk_duration_s,
        app->tally->radios,
        app->tally->radios_capped,
        app->tally->total);
    uint32_t other = 0;
    for(uint8_t i = 0; i < app->rules->brand_count; i++) {
        if(app->rules->brands[i].other) {
            other += app->tally->counts[i];
        } else {
            receipt_view_add_row(
                app->receipt_view,
                app->rules->brands[i].name,
                app->tally->counts[i],
                ReceiptTrendNone);
        }
    }
    if(rules_has_other(app->rules)) {
        receipt_view_add_row(app->receipt_view, "Other", other, ReceiptTrendNone);
    }
    furi_mutex_release(app->tally_mutex);

    app->receipt_saved = false;
    app->receipt_is_history = false;
    show_view(app, ViewReceipt);
    if(reveal) receipt_view_play_reveal(app->receipt_view);
}

static bool tally_save(Doorstep* app) {
    // One line: per-brand counts and duration. No date, no clock time.
    FuriString* line = furi_string_alloc();
    uint32_t t = app->walk_duration_s;
    furi_string_printf(
        line,
        "duration=%02lu:%02lu:%02lu",
        (unsigned long)(t / 3600),
        (unsigned long)((t / 60) % 60),
        (unsigned long)(t % 60));

    uint32_t other = 0;
    for(uint8_t i = 0; i < app->rules->brand_count; i++) {
        if(app->rules->brands[i].other) {
            other += app->tally->counts[i];
        } else {
            // Keep "key=value" parseable if a brand name has spaces or '='
            char name[RULES_NAME_LEN];
            strlcpy(name, app->rules->brands[i].name, sizeof(name));
            for(char* c = name; *c; c++)
                if(*c == ' ' || *c == '=') *c = '_';
            furi_string_cat_printf(line, " %s=%u", name, app->tally->counts[i]);
        }
    }
    if(rules_has_other(app->rules)) furi_string_cat_printf(line, " Other=%lu", (unsigned long)other);
    furi_string_cat_printf(
        line,
        " Radios=%lu%s Total=%lu\n",
        (unsigned long)app->tally->radios,
        app->tally->radios_capped ? "+" : "",
        (unsigned long)app->tally->total);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, TALLIES_PATH, FSAM_WRITE, FSOM_OPEN_APPEND);
    if(ok) {
        size_t len = furi_string_size(line);
        ok = storage_file_write(file, furi_string_get_cstr(line), len) == len;
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    furi_string_free(line);
    return ok;
}

// ---------------------------------------------------------------- saved walks

typedef struct {
    uint16_t first; // oldest index listed
    uint16_t count; // walks collected
    uint16_t indexes[SAVED_LIST_MAX];
    char labels[SAVED_LIST_MAX][48];
} SavedList;

static void saved_list_collect(uint16_t index, const HistoryWalk* walk, void* context) {
    SavedList* list = context;
    if(index < list->first) return;
    uint16_t slot = list->count++;
    uint32_t t = walk->duration_s;
    list->indexes[slot] = index;
    snprintf(
        list->labels[slot],
        sizeof(list->labels[slot]),
        "#%u  %lu:%02lu:%02lu  %lu",
        index + 1,
        (unsigned long)(t / 3600),
        (unsigned long)((t / 60) % 60),
        (unsigned long)(t % 60),
        (unsigned long)walk->total);
}

static void saved_item_callback(void* context, uint32_t index);

/** Refill the Saved Walks list. Returns how many walks are saved. */
static uint16_t saved_rebuild(Doorstep* app) {
    uint16_t total = history_for_each(TALLIES_PATH, NULL, NULL);
    if(total == 0) return 0;

    SavedList* list = malloc(sizeof(SavedList));
    memset(list, 0, sizeof(SavedList));
    list->first = total > SAVED_LIST_MAX ? total - SAVED_LIST_MAX : 0;
    history_for_each(TALLIES_PATH, saved_list_collect, list);

    submenu_reset(app->saved_menu);
    submenu_set_header(app->saved_menu, "Saved Walks");
    submenu_add_item(app->saved_menu, "Lifetime totals", LIFETIME_INDEX, saved_item_callback, app);
    // Newest first
    for(int i = list->count - 1; i >= 0; i--) {
        submenu_add_item(
            app->saved_menu, list->labels[i], list->indexes[i], saved_item_callback, app);
    }
    free(list);
    return total;
}

static void saved_open(Doorstep* app) {
    if(saved_rebuild(app) == 0) {
        show_popup(app, "No saved walks", "OK on a receipt\nsaves one", 2000, ViewMenu);
        return;
    }
    show_view(app, ViewSaved);
}

typedef struct {
    uint16_t walks;
    uint32_t duration_s;
    uint32_t total;
    char names[HISTORY_MAX_ROWS][RULES_NAME_LEN];
    uint32_t sums[HISTORY_MAX_ROWS];
    uint8_t name_count;
} LifetimeTotals;

static void lifetime_collect(uint16_t index, const HistoryWalk* walk, void* context) {
    UNUSED(index);
    LifetimeTotals* lt = context;
    lt->walks++;
    lt->duration_s += walk->duration_s;
    lt->total += walk->total;
    for(uint8_t r = 0; r < walk->row_count; r++) {
        uint8_t k = 0;
        while(k < lt->name_count && strcmp(lt->names[k], walk->rows[r].name) != 0)
            k++;
        if(k == lt->name_count) {
            if(lt->name_count == HISTORY_MAX_ROWS) continue;
            strlcpy(lt->names[k], walk->rows[r].name, RULES_NAME_LEN);
            lt->name_count++;
        }
        lt->sums[k] += walk->rows[r].value;
    }
}

static void saved_show_lifetime(Doorstep* app) {
    LifetimeTotals* lt = malloc(sizeof(LifetimeTotals));
    memset(lt, 0, sizeof(LifetimeTotals));
    history_for_each(TALLIES_PATH, lifetime_collect, lt);

    char title[RECEIPT_TITLE_LEN];
    snprintf(title, sizeof(title), "ALL %u WALK%s", lt->walks, lt->walks == 1 ? "" : "S");
    // Radios can't be summed: the same radio heard on two walks would count twice
    receipt_view_reset(
        app->receipt_view, title, lt->duration_s, RECEIPT_RADIOS_UNKNOWN, false, lt->total);
    for(uint8_t k = 0; k < lt->name_count; k++) {
        receipt_view_add_row(app->receipt_view, lt->names[k], lt->sums[k], ReceiptTrendNone);
    }
    free(lt);

    app->receipt_is_history = true;
    app->history_index = LIFETIME_INDEX;
    show_view(app, ViewReceipt);
}

#define AVG_MAX_BRANDS HISTORY_MAX_ROWS

typedef struct {
    uint16_t target;
    bool found;
    HistoryWalk walk;
    // Per-brand average of per-hour rates across saved walks
    char names[AVG_MAX_BRANDS][RULES_NAME_LEN];
    float rate_sum[AVG_MAX_BRANDS];
    uint16_t rate_n[AVG_MAX_BRANDS];
    uint8_t name_count;
} SavedDetail;

static void saved_detail_collect(uint16_t index, const HistoryWalk* walk, void* context) {
    SavedDetail* d = context;
    if(index == d->target) {
        d->walk = *walk;
        d->found = true;
    }
    if(walk->duration_s == 0) return;
    float hours = walk->duration_s / 3600.0f;
    for(uint8_t r = 0; r < walk->row_count; r++) {
        uint8_t k = 0;
        while(k < d->name_count && strcmp(d->names[k], walk->rows[r].name) != 0)
            k++;
        if(k == d->name_count) {
            if(d->name_count == AVG_MAX_BRANDS) continue;
            strlcpy(d->names[k], walk->rows[r].name, RULES_NAME_LEN);
            d->name_count++;
        }
        d->rate_sum[k] += walk->rows[r].value / hours;
        d->rate_n[k]++;
    }
}

static ReceiptTrend saved_trend(const SavedDetail* d, const HistoryRow* row) {
    if(d->walk.duration_s == 0) return ReceiptTrendNone;
    for(uint8_t k = 0; k < d->name_count; k++) {
        if(strcmp(d->names[k], row->name) != 0) continue;
        if(d->rate_n[k] < 2) return ReceiptTrendNone; // nothing to compare against
        float avg = d->rate_sum[k] / d->rate_n[k];
        float rate = row->value / (d->walk.duration_s / 3600.0f);
        if(rate > avg * (1.0f + TREND_BAND) && rate > 0) return ReceiptTrendUp;
        if(rate < avg * (1.0f - TREND_BAND)) return ReceiptTrendDown;
        return ReceiptTrendNone;
    }
    return ReceiptTrendNone;
}

static void saved_item_callback(void* context, uint32_t index) {
    Doorstep* app = context;
    if(index == LIFETIME_INDEX) {
        saved_show_lifetime(app);
        return;
    }
    SavedDetail* d = malloc(sizeof(SavedDetail));
    memset(d, 0, sizeof(SavedDetail));
    d->target = (uint16_t)index;
    history_for_each(TALLIES_PATH, saved_detail_collect, d);

    if(d->found) {
        char title[RECEIPT_TITLE_LEN];
        snprintf(title, sizeof(title), "WALK #%lu", (unsigned long)index + 1);
        receipt_view_reset(
            app->receipt_view,
            title,
            d->walk.duration_s,
            d->walk.radios == HISTORY_RADIOS_UNKNOWN ? RECEIPT_RADIOS_UNKNOWN : d->walk.radios,
            d->walk.radios_capped,
            d->walk.total);
        for(uint8_t r = 0; r < d->walk.row_count; r++) {
            receipt_view_add_row(
                app->receipt_view,
                d->walk.rows[r].name,
                d->walk.rows[r].value,
                saved_trend(d, &d->walk.rows[r]));
        }
        app->receipt_is_history = true;
        app->history_index = index;
        // No reveal here: browsing is frequent, the print moment belongs to a fresh walk
        show_view(app, ViewReceipt);
    } else {
        show_popup(app, "Walk not found", "tallies.txt changed?", 2000, ViewSaved);
    }
    free(d);
}

// ---------------------------------------------------------------- callbacks

static void walk_pause_callback(void* context) {
    Doorstep* app = context;
    uint32_t now = furi_get_tick();
    if(app->paused) {
        app->paused_ticks += now - app->pause_started;
        app->paused = false;
    } else {
        app->pause_started = now;
        app->paused = true;
    }
    marauder_set_paused(app->marauder, app->paused);
    notification_message(app->notifications, &sequence_single_vibro);
    ui_timer_callback(app);
}

static void walk_back_callback(void* context) {
    Doorstep* app = context;
    show_dialog(
        app, DialogEndWalk, "End walk?", "Your tally prints\non the receipt.", "Keep going", NULL, "End");
}

static void receipt_callback(ReceiptEvent event, void* context) {
    Doorstep* app = context;
    if(app->receipt_is_history) {
        // Saved walks: Back returns to the list, OK offers to delete the walk
        if(event == ReceiptEventDone) {
            show_view(app, ViewSaved);
        } else if(app->history_index != LIFETIME_INDEX) {
            static char header[24];
            snprintf(
                header, sizeof(header), "Delete walk #%lu?", (unsigned long)app->history_index + 1);
            show_dialog(
                app,
                DialogDeleteWalk,
                header,
                "Removes it from\ntallies.txt.",
                "Keep",
                NULL,
                "Delete");
        }
        return;
    }
    if(event == ReceiptEventSave) {
        if(app->receipt_saved) {
            show_popup(app, "Already saved", NULL, 1000, ViewReceipt);
        } else {
            show_dialog(
                app,
                DialogSave,
                "Save tally?",
                "Adds one line to\ntallies.txt. No date.",
                "Cancel",
                NULL,
                "Save");
        }
    } else if(app->receipt_saved) {
        show_view(app, ViewMenu);
    } else {
        show_dialog(
            app,
            DialogDiscard,
            "Discard receipt?",
            "This tally isn't saved.\nOK on the receipt saves.",
            "Keep",
            NULL,
            "Discard");
    }
}

static void dialog_callback(DialogExResult result, void* context) {
    Doorstep* app = context;
    switch(app->dialog_kind) {
    case DialogEndWalk:
        if(result == DialogExResultRight)
            walk_stop(app, true);
        else if(result == DialogExResultLeft)
            show_view(app, ViewWalk);
        break;
    case DialogSave:
        if(result == DialogExResultRight) {
            if(tally_save(app)) {
                app->receipt_saved = true;
                notification_message(app->notifications, &sequence_success);
                show_popup(app, "Saved", "tallies.txt", 1200, ViewReceipt);
            } else {
                show_popup(app, "Save failed", "Check the SD card", 2000, ViewReceipt);
            }
        } else if(result == DialogExResultLeft) {
            show_view(app, ViewReceipt);
        }
        break;
    case DialogDiscard:
        if(result == DialogExResultRight)
            show_view(app, ViewMenu);
        else if(result == DialogExResultLeft)
            show_view(app, ViewReceipt);
        break;
    case DialogDeleteWalk:
        if(result == DialogExResultRight) {
            if(!history_delete(TALLIES_PATH, (uint16_t)app->history_index)) {
                show_popup(app, "Delete failed", "Check the SD card", 2000, ViewReceipt);
            } else if(saved_rebuild(app) > 0) {
                show_popup(app, "Deleted", NULL, 1000, ViewSaved);
            } else {
                show_popup(app, "Deleted", "No saved walks left", 1500, ViewMenu);
            }
        } else if(result == DialogExResultLeft) {
            show_view(app, ViewReceipt);
        }
        break;
    case DialogRules:
        if(result == DialogExResultCenter) {
            rules_reload(app);
            notification_message(app->notifications, &sequence_blink_green_10);
            dialog_ex_set_text(app->dialog, app->rules_text, 64, 30, AlignCenter, AlignCenter);
        }
        break;
    }
}

static void menu_callback(void* context, uint32_t index) {
    Doorstep* app = context;
    switch(index) {
    case MenuStartWalk:
        walk_start(app);
        break;
    case MenuSaved:
        saved_open(app);
        break;
    case MenuRules:
        show_dialog(app, DialogRules, "Rules", app->rules_text, NULL, "Reload", NULL);
        break;
    case MenuSettings:
        show_view(app, ViewSettings);
        break;
    case MenuAbout:
        show_view(app, ViewAbout);
        break;
    }
}

static int32_t check_worker(void* context) {
    Doorstep* app = context;
    bool ok = marauder_check(
        app->marauder,
        CHECK_TIMEOUT,
        app->check_version,
        sizeof(app->check_version),
        &app->check_scanall);
    view_dispatcher_send_custom_event(app->view_dispatcher, ok ? EventCheckOk : EventCheckFail);
    return 0;
}

static bool custom_event_callback(void* context, uint32_t event) {
    Doorstep* app = context;
    if(event >= EventPopupDone) {
        // Ignore a late timeout from a popup that was already dismissed or replaced
        if(app->current_view == ViewPopup && !app->checking &&
           (event - EventPopupDone) == app->popup_generation) {
            show_view(app, app->popup_return);
        }
        return true;
    }

    if(event == EventLowBattery) {
        if(!app->walking) return true;
        walk_stop(app, false);
        if(tally_save(app)) {
            app->receipt_saved = true;
            show_popup(app, "Battery low", "Walk ended and saved", 3000, ViewReceipt);
        } else {
            show_popup(app, "Battery low", "Walk ended. Save failed", 3000, ViewReceipt);
        }
        notification_message(app->notifications, &sequence_error);
        return true;
    }

    furi_thread_join(app->check_thread);
    app->checking = false;
    if(event == EventCheckOk) {
        notification_message(app->notifications, &sequence_success);
        static char text[64];
        snprintf(
            text,
            sizeof(text),
            "%s%s\n%s",
            app->check_version[0] ? "Marauder " : "Version not reported",
            app->check_version,
            app->check_scanall ? "Walks scan ALL" : "No scanall: STA/AP");
        show_popup(app, "Board OK", text, 3000, ViewSettings);
        return true;
    } else if(event == EventCheckFail) {
        show_popup(app, "No reply", "Check the board is\nseated and on Marauder", 2500, ViewSettings);
        return true;
    }
    return false;
}

static bool navigation_callback(void* context) {
    // Back pressed and not handled by the current view
    Doorstep* app = context;
    switch(app->current_view) {
    case ViewMenu:
        return false; // exit the app
    case ViewSettings:
        settings_save(app);
        show_view(app, ViewMenu);
        break;
    case ViewDialog:
        // Back on a dialog behaves like its "keep / cancel" answer
        if(app->dialog_kind == DialogEndWalk)
            show_view(app, ViewWalk);
        else if(app->dialog_kind == DialogRules)
            show_view(app, ViewMenu);
        else
            show_view(app, ViewReceipt);
        break;
    case ViewPopup:
        if(!app->checking) show_view(app, app->popup_return);
        break;
    default:
        show_view(app, ViewMenu);
        break;
    }
    return true;
}

// ---------------------------------------------------------------- settings list

#define SETTING_CHANGED(fn, field, labels)                          \
    static void fn(VariableItem* item) {                            \
        Doorstep* app = variable_item_get_context(item);            \
        uint8_t i = variable_item_get_current_value_index(item);    \
        app->settings.field = i;                                    \
        variable_item_set_current_value_text(item, labels[i]);      \
    }

SETTING_CHANGED(pass_changed, pass, pass_labels)
SETTING_CHANGED(mode_changed, mode, mode_labels)
SETTING_CHANGED(range_changed, range, range_labels)
SETTING_CHANGED(buzz_changed, buzz, buzz_labels)

enum {
    SettingPass,
    SettingMode,
    SettingRange,
    SettingBuzz,
    SettingCheck,
};

static void settings_enter_callback(void* context, uint32_t index) {
    Doorstep* app = context;
    if(index != SettingCheck) return;
    if(app->checking) return;
    // No timeout and Back is held (see navigation_callback) until the check reports,
    // so a walk can't start while the check owns the UART
    show_popup(app, "Checking board...", NULL, 0, ViewSettings);
    app->checking = true;
    furi_thread_start(app->check_thread);
}

static void settings_add(
    Doorstep* app,
    const char* label,
    uint8_t count,
    uint8_t current,
    VariableItemChangeCallback cb,
    const char* const* labels) {
    VariableItem* item = variable_item_list_add(app->settings_list, label, count, cb, app);
    variable_item_set_current_value_index(item, current);
    variable_item_set_current_value_text(item, labels[current]);
}

static void settings_build(Doorstep* app) {
    settings_add(app, "Scan pass", COUNT_OF(pass_labels), app->settings.pass, pass_changed, pass_labels);
    settings_add(app, "Mode", COUNT_OF(mode_labels), app->settings.mode, mode_changed, mode_labels);
    settings_add(app, "Range", COUNT_OF(range_labels), app->settings.range, range_changed, range_labels);
    settings_add(app, "Buzz on new", COUNT_OF(buzz_labels), app->settings.buzz, buzz_changed, buzz_labels);
    variable_item_list_add(app->settings_list, "Check board", 0, NULL, app);
    variable_item_list_set_enter_callback(app->settings_list, settings_enter_callback, app);
}

// ---------------------------------------------------------------- lifecycle

static const char about_text[] =
    "Doorstep v" DOORSTEP_VERSION "\n\n"
    "Counts camera-brand Wi-Fi radios within range of you on a walk.\n\n"
    "Shows counts per brand and total walk time. Nothing else: no addresses, "
    "network names, locations or detection times are kept or shown.\n\n"
    "Range: Near ~ the doorstep, Street ~ both sides of the street, Max = all heard. "
    "Clients are never range-filtered (Marauder reports no signal for them).\n\n"
    "Hold OK on the walk screen to pause or resume; paused time isn't counted.\n\n"
    "Buzz on new: at most one buzz every 5 s.\n\n"
    "Three long buzzes + red light: memory full, no more cameras can be counted.\n\n"
    "Battery under 10% (not charging): the walk ends and saves itself.\n\n"
    "Saved Walks: OK on a walk deletes it. Lifetime totals adds them all up.\n\n"
    "Radios heard: every distinct radio in range, any vendor, for context.\n\n"
    "Saved Walks: arrows mark brands above or below your average per hour.\n\n"
    "In range is not in view. Counts are rough in both directions.\n\n"
    "Needs the WiFi Dev Board running Marauder.";

static Doorstep* doorstep_alloc(void) {
    Doorstep* app = malloc(sizeof(Doorstep));
    memset(app, 0, sizeof(Doorstep));

    app->rules = malloc(sizeof(Rules));
    app->tally = tally_alloc();
    app->tally_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->marauder = marauder_alloc();
    app->ui_timer = furi_timer_alloc(ui_timer_callback, FuriTimerTypePeriodic, app);
    app->check_thread = furi_thread_alloc_ex("DoorstepCheck", 1024, check_worker, app);

    settings_load(app);
    rules_install_default();
    rules_reload(app);

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, navigation_callback);

    // Menu
    app->menu = submenu_alloc();
    submenu_set_header(app->menu, "Doorstep");
    submenu_add_item(app->menu, "Start Walk", MenuStartWalk, menu_callback, app);
    submenu_add_item(app->menu, "Saved Walks", MenuSaved, menu_callback, app);
    submenu_add_item(app->menu, "Rules", MenuRules, menu_callback, app);
    submenu_add_item(app->menu, "Settings", MenuSettings, menu_callback, app);
    submenu_add_item(app->menu, "About", MenuAbout, menu_callback, app);
    view_dispatcher_add_view(app->view_dispatcher, ViewMenu, submenu_get_view(app->menu));

    // Walk
    app->walk_view = walk_view_alloc();
    walk_view_set_back_callback(app->walk_view, walk_back_callback, app);
    walk_view_set_pause_callback(app->walk_view, walk_pause_callback);
    view_dispatcher_add_view(app->view_dispatcher, ViewWalk, walk_view_get_view(app->walk_view));

    // Receipt
    app->receipt_view = receipt_view_alloc();
    receipt_view_set_callback(app->receipt_view, receipt_callback, app);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewReceipt, receipt_view_get_view(app->receipt_view));

    // Dialog
    app->dialog = dialog_ex_alloc();
    dialog_ex_set_result_callback(app->dialog, dialog_callback);
    view_dispatcher_add_view(app->view_dispatcher, ViewDialog, dialog_ex_get_view(app->dialog));

    // Popup
    app->popup = popup_alloc();
    view_dispatcher_add_view(app->view_dispatcher, ViewPopup, popup_get_view(app->popup));

    // Settings
    app->settings_list = variable_item_list_alloc();
    settings_build(app);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewSettings, variable_item_list_get_view(app->settings_list));

    // Saved walks
    app->saved_menu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, ViewSaved, submenu_get_view(app->saved_menu));

    // About
    app->about = text_box_alloc();
    text_box_set_font(app->about, TextBoxFontText);
    text_box_set_text(app->about, about_text);
    view_dispatcher_add_view(app->view_dispatcher, ViewAbout, text_box_get_view(app->about));

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    show_view(app, ViewMenu);
    return app;
}

static void doorstep_free(Doorstep* app) {
    furi_timer_stop(app->ui_timer);
    furi_timer_free(app->ui_timer);
    furi_thread_join(app->check_thread);
    furi_thread_free(app->check_thread);
    marauder_free(app->marauder);

    view_dispatcher_remove_view(app->view_dispatcher, ViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewWalk);
    view_dispatcher_remove_view(app->view_dispatcher, ViewReceipt);
    view_dispatcher_remove_view(app->view_dispatcher, ViewDialog);
    view_dispatcher_remove_view(app->view_dispatcher, ViewPopup);
    view_dispatcher_remove_view(app->view_dispatcher, ViewSettings);
    view_dispatcher_remove_view(app->view_dispatcher, ViewAbout);
    view_dispatcher_remove_view(app->view_dispatcher, ViewSaved);
    submenu_free(app->menu);
    walk_view_free(app->walk_view);
    receipt_view_free(app->receipt_view);
    dialog_ex_free(app->dialog);
    popup_free(app->popup);
    variable_item_list_free(app->settings_list);
    text_box_free(app->about);
    submenu_free(app->saved_menu);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    tally_free(app->tally);
    furi_mutex_free(app->tally_mutex);
    free(app->rules);
    free(app);
}

int32_t doorstep_app(void* p) {
    UNUSED(p);
    Doorstep* app = doorstep_alloc();
    view_dispatcher_run(app->view_dispatcher);
    doorstep_free(app);
    return 0;
}
