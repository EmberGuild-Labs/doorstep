#include "history.h"
#include "chars.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>
#include <string.h>
#include <stdlib.h>

static bool parse_duration(const char* s, uint32_t* out) {
    // HH:MM:SS (hours may exceed two digits)
    uint32_t parts[3] = {0};
    int part = 0;
    bool digit = false;
    for(; *s; s++) {
        if(ch_is_digit(*s)) {
            parts[part] = parts[part] * 10 + (uint32_t)(*s - '0');
            digit = true;
        } else if(*s == ':' && part < 2 && digit) {
            part++;
            digit = false;
        } else {
            return false;
        }
    }
    if(part != 2 || !digit) return false;
    *out = parts[0] * 3600 + parts[1] * 60 + parts[2];
    return true;
}

static bool parse_count(const char* s, uint32_t* out, bool* plus) {
    if(!ch_is_digit(*s)) return false;
    uint32_t v = 0;
    while(ch_is_digit(*s))
        v = v * 10 + (uint32_t)(*s++ - '0');
    if(plus) *plus = (*s == '+');
    *out = v;
    return true;
}

bool history_parse_line(const char* line, HistoryWalk* walk) {
    memset(walk, 0, sizeof(HistoryWalk));
    walk->radios = HISTORY_RADIOS_UNKNOWN;
    bool has_duration = false, has_total = false;

    size_t len = strlen(line);
    char* buf = malloc(len + 1);
    memcpy(buf, line, len + 1);

    char* save = NULL;
    for(char* tok = strtok_r(buf, " \t\r\n", &save); tok; tok = strtok_r(NULL, " \t\r\n", &save)) {
        char* eq = strchr(tok, '=');
        if(!eq || eq == tok) continue;
        *eq = '\0';
        const char* key = tok;
        const char* value = eq + 1;

        if(strcmp(key, "duration") == 0) {
            has_duration = parse_duration(value, &walk->duration_s);
        } else if(strcmp(key, "Total") == 0) {
            has_total = parse_count(value, &walk->total, NULL);
        } else if(strcmp(key, "Radios") == 0) {
            if(!parse_count(value, &walk->radios, &walk->radios_capped)) {
                walk->radios = HISTORY_RADIOS_UNKNOWN;
            }
        } else if(walk->row_count < HISTORY_MAX_ROWS) {
            HistoryRow* row = &walk->rows[walk->row_count];
            if(parse_count(value, &row->value, NULL)) {
                strlcpy(row->name, key, sizeof(row->name));
                walk->row_count++;
            }
        }
    }

    free(buf);
    return has_duration && has_total;
}

uint16_t history_for_each(const char* path, HistoryWalkCallback callback, void* context) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    FuriString* line = furi_string_alloc();
    HistoryWalk* walk = malloc(sizeof(HistoryWalk));
    uint16_t count = 0;

    if(file_stream_open(stream, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        while(stream_read_line(stream, line) && count < UINT16_MAX) {
            if(history_parse_line(furi_string_get_cstr(line), walk)) {
                if(callback) callback(count, walk, context);
                count++;
            }
        }
    }

    free(walk);
    furi_string_free(line);
    file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
    return count;
}

bool history_delete(const char* path, uint16_t index) {
    // Copy every line except the index-th walk to a temp file, then swap it in.
    // Lines that aren't walks are kept as they are.
    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* in = file_stream_alloc(storage);
    File* out = storage_file_alloc(storage);
    FuriString* line = furi_string_alloc();
    FuriString* tmp_path = furi_string_alloc_printf("%s.tmp", path);
    HistoryWalk* walk = malloc(sizeof(HistoryWalk));
    bool ok = false;
    bool removed = false;

    if(file_stream_open(in, path, FSAM_READ, FSOM_OPEN_EXISTING) &&
       storage_file_open(out, furi_string_get_cstr(tmp_path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ok = true;
        uint16_t count = 0;
        while(stream_read_line(in, line)) {
            if(history_parse_line(furi_string_get_cstr(line), walk)) {
                if(count++ == index) {
                    removed = true;
                    continue;
                }
            }
            // Keep every line newline-terminated so later appends stay separate
            if(!furi_string_end_with_str(line, "\n")) furi_string_push_back(line, '\n');
            size_t len = furi_string_size(line);
            if(storage_file_write(out, furi_string_get_cstr(line), len) != len) {
                ok = false;
                break;
            }
        }
    }
    storage_file_close(out);
    file_stream_close(in);

    if(ok && removed) {
        ok = storage_common_remove(storage, path) == FSE_OK &&
             storage_common_rename(storage, furi_string_get_cstr(tmp_path), path) == FSE_OK;
    } else {
        storage_common_remove(storage, furi_string_get_cstr(tmp_path));
        ok = false;
    }

    free(walk);
    furi_string_free(tmp_path);
    furi_string_free(line);
    storage_file_free(out);
    stream_free(in);
    furi_record_close(RECORD_STORAGE);
    return ok;
}
