#include "tally.h"

#include <furi.h>
#include <furi_hal_random.h>
#include "chars.h"
#include <string.h>
#include <stdlib.h>

#define MAC_STR_LEN     17
#define MAX_MACS_PER_LN 4

Tally* tally_alloc(void) {
    Tally* tally = malloc(sizeof(Tally));
    memset(tally, 0, sizeof(Tally));
    tally->hashes = malloc(sizeof(uint32_t) * TALLY_MAX_DEVICES);
    return tally;
}

void tally_free(Tally* tally) {
    tally_forget_devices(tally);
    free(tally->hashes);
    free(tally);
}

void tally_reset(Tally* tally) {
    tally_forget_devices(tally);
    memset(tally->counts, 0, sizeof(tally->counts));
    tally->total = 0;
    tally->radios = 0;
    tally->full = false;
    tally->radios_capped = false;
    // Salt 0 means "no walk in progress", so never use it
    do {
        tally->salt = furi_hal_random_get();
    } while(tally->salt == 0);
}

void tally_forget_devices(Tally* tally) {
    memset(tally->hashes, 0, sizeof(uint32_t) * TALLY_MAX_DEVICES);
    tally->hash_count = 0;
    tally->salt = 0;
}

static uint32_t device_hash(uint32_t salt, const uint8_t mac[6]) {
    // FNV-1a seeded with the salt, then a murmur3 finalizer
    uint32_t h = 2166136261u ^ salt;
    for(int i = 0; i < 6; i++) {
        h ^= mac[i];
        h *= 16777619u;
    }
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

// Domain tags: the same device is hashed once as a radio and once as a camera
#define DOMAIN_CAMERA 0x00000000u
#define DOMAIN_RADIO  0x5bd1e995u

/** Insert hash; returns true if it was new. limit caps how full the list may get. */
static bool hash_insert(Tally* tally, uint32_t h, uint16_t limit) {
    size_t lo = 0, hi = tally->hash_count;
    while(lo < hi) {
        size_t mid = (lo + hi) / 2;
        if(tally->hashes[mid] < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    if(lo < tally->hash_count && tally->hashes[lo] == h) return false;
    if(tally->hash_count >= limit) {
        // Full: stop adding new devices rather than evicting old ones
        if(limit == TALLY_MAX_DEVICES)
            tally->full = true;
        else
            tally->radios_capped = true;
        return false;
    }
    memmove(
        &tally->hashes[lo + 1], &tally->hashes[lo], (tally->hash_count - lo) * sizeof(uint32_t));
    tally->hashes[lo] = h;
    tally->hash_count++;
    return true;
}

static bool parse_mac_at(const char* p, uint8_t mac[6]) {
    for(int i = 0; i < 6; i++) {
        int hi = ch_hexval(p[i * 3]);
        int lo = ch_hexval(p[i * 3 + 1]);
        if(hi < 0 || lo < 0) return false;
        if(i < 5 && p[i * 3 + 2] != ':') return false;
        mac[i] = (uint8_t)((hi << 4) | lo);
    }
    // Must not be part of a longer hex run
    char after = p[MAC_STR_LEN];
    return !(after == ':' || ch_is_xdigit(after));
}

/**
 * Find RSSI on the line. Accepts "RSSI: -60" / "rssi=-60", or Marauder's AP
 * format where the line starts with the value: "-67 Ch: 6 aa:bb:...".
 * Station lines ("ap: X -> sta: Y") carry no signal value.
 */
static bool parse_rssi(const char* line, int16_t* out) {
    const char* p = strcasestr(line, "rssi");
    if(p) {
        p += 4;
        while(*p == ':' || *p == '=' || *p == ' ')
            p++;
        if(*p != '-' && !ch_is_digit(*p)) return false;
        *out = (int16_t)atoi(p);
        return true;
    }

    p = line;
    while(*p == ' ')
        p++;
    if(*p != '-' || !ch_is_digit(p[1])) return false;
    const char* q = p + 1;
    while(ch_is_digit(*q))
        q++;
    if(strncmp(q, " Ch:", 4) != 0) return false;
    *out = (int16_t)atoi(p);
    return true;
}

/** Copy the network name after "ESSID:" (or "SSID:") into out. */
static bool parse_essid(const char* line, char* out, size_t out_len) {
    const char* p = strcasestr(line, "essid:");
    if(p) {
        p += 6;
    } else {
        p = strcasestr(line, "ssid:");
        if(!p) return false;
        p += 5;
    }
    while(*p == ' ')
        p++;
    size_t n = 0;
    while(p[n] && p[n] != '\r' && p[n] != '\n' && n + 1 < out_len) {
        out[n] = p[n];
        n++;
    }
    while(n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
    return n > 0;
}

uint8_t tally_feed_line(Tally* tally, const Rules* rules, const char* line, int16_t min_rssi) {
    if(!tally->salt) return 0; // no walk in progress

    int16_t rssi;
    if(min_rssi > TALLY_RSSI_ANY && parse_rssi(line, &rssi) && rssi < min_rssi) {
        return 0; // too far away; the value is discarded here
    }

    char essid[33];
    uint8_t name_brand = RULES_NO_MATCH;
    if(parse_essid(line, essid, sizeof(essid))) {
        name_brand = rules_match_name(rules, essid);
    }

    uint8_t added = 0;
    uint8_t macs_seen = 0;
    size_t len = strlen(line);
    for(size_t i = 0; i + MAC_STR_LEN <= len && macs_seen < MAX_MACS_PER_LN; i++) {
        if(i > 0 && (line[i - 1] == ':' || ch_is_xdigit(line[i - 1]))) continue;
        uint8_t mac[6];
        if(!parse_mac_at(&line[i], mac)) continue;
        macs_seen++;
        i += MAC_STR_LEN - 1;

        if(mac[0] & 0x01) continue; // multicast / broadcast

        if(hash_insert(
               tally,
               device_hash(tally->salt ^ DOMAIN_RADIO, mac),
               TALLY_MAX_DEVICES - TALLY_CAMERA_RESERVE)) {
            tally->radios++;
        }

        uint8_t brand = RULES_NO_MATCH;
        // Locally administered (randomized) addresses carry no vendor prefix
        if(!(mac[0] & 0x02)) {
            uint32_t oui = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
            brand = rules_match_prefix(rules, oui);
        }
        // Network name applies to the first address on the line (the BSSID)
        if(macs_seen == 1 && name_brand < brand) brand = name_brand;
        if(brand == RULES_NO_MATCH) continue;

        if(hash_insert(tally, device_hash(tally->salt ^ DOMAIN_CAMERA, mac), TALLY_MAX_DEVICES)) {
            tally->counts[brand]++;
            tally->total++;
            added++;
        }
    }

    // Scrub the stack copy of the network name
    memset(essid, 0, sizeof(essid));
    return added;
}
