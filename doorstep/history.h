#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "rules.h"

/** Radios value for walks saved before the radio count existed. */
#define HISTORY_RADIOS_UNKNOWN UINT32_MAX

#define HISTORY_MAX_ROWS (RULES_MAX_BRANDS + 1) // brands + Other

typedef struct {
    char name[RULES_NAME_LEN];
    uint32_t value;
} HistoryRow;

/** One saved walk: a line of tallies.txt. */
typedef struct {
    uint32_t duration_s;
    uint32_t total;
    uint32_t radios;
    bool radios_capped;
    HistoryRow rows[HISTORY_MAX_ROWS];
    uint8_t row_count;
} HistoryWalk;

/**
 * Parse "duration=HH:MM:SS Ring=23 ... Other=3 Radios=812 Total=147".
 * Unknown tokens are ignored. Returns false if there is no duration or Total.
 */
bool history_parse_line(const char* line, HistoryWalk* walk);

typedef void (*HistoryWalkCallback)(uint16_t index, const HistoryWalk* walk, void* context);

/** Call callback for every parseable line, in file order. Returns the count. */
uint16_t history_for_each(const char* path, HistoryWalkCallback callback, void* context);

/** Remove the index-th walk (same numbering as history_for_each). Other lines are kept. */
bool history_delete(const char* path, uint16_t index);
