#pragma once

#include "rules.h"

#define TALLY_MAX_DEVICES 4000
/** Slots only cameras may use, so a busy street can't crowd them out. */
#define TALLY_CAMERA_RESERVE 500

/** Signal floor that disables the range filter. */
#define TALLY_RSSI_ANY (-128)

/**
 * Per-walk tally. Devices are remembered only as salted 32-bit hashes
 * (sorted, for binary search); raw addresses are never stored.
 */
typedef struct {
    uint32_t salt;
    uint32_t* hashes; // TALLY_MAX_DEVICES entries
    uint16_t hash_count;
    bool full; // no room left for new cameras
    bool radios_capped; // radio count stopped growing (shown as "N+")
    uint16_t counts[RULES_MAX_BRANDS];
    uint32_t total;
    uint32_t radios; // distinct unicast radios heard (any vendor), for context
} Tally;

Tally* tally_alloc(void);
void tally_free(Tally* tally);

/** Start a walk: new random salt, empty hash list, zero counts. */
void tally_reset(Tally* tally);

/** Wipe the hash list and salt; counts and total are kept. */
void tally_forget_devices(Tally* tally);

/**
 * Parse one line of Marauder output and count any new matching devices.
 * min_rssi: lines reporting a weaker RSSI are ignored (TALLY_RSSI_ANY = no filter).
 * Returns the number of newly counted devices.
 */
uint8_t tally_feed_line(Tally* tally, const Rules* rules, const char* line, int16_t min_rssi);
