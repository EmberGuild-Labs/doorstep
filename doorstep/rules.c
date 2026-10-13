#include "rules.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>
#include <string.h>
#include "chars.h"

static void str_lower(char* s) {
    for(; *s; s++)
        *s = ch_lower(*s);
}

static bool parse_hex24(const char* tok, uint32_t* out) {
    // Accept "286FB9", "28:6F:B9", "28-6F-B9"
    uint32_t v = 0;
    int digits = 0;
    for(const char* p = tok; *p; p++) {
        if(*p == ':' || *p == '-') continue;
        if(!ch_is_xdigit(*p)) return false;
        if(digits == 6) return false;
        v = (v << 4) | (uint32_t)ch_hexval(*p);
        digits++;
    }
    if(digits != 6) return false;
    *out = v;
    return true;
}

static uint8_t find_or_add_brand(Rules* rules, const char* full_name, bool other) {
    // Compare the name as stored (truncated), so long names still merge
    char name[RULES_NAME_LEN];
    strlcpy(name, full_name, sizeof(name));
    for(uint8_t i = 0; i < rules->brand_count; i++) {
        if(strcasecmp(rules->brands[i].name, name) == 0) return i;
    }
    if(rules->brand_count >= RULES_MAX_BRANDS) return RULES_NO_MATCH;
    RuleBrand* b = &rules->brands[rules->brand_count];
    strlcpy(b->name, name, sizeof(b->name));
    b->other = other;
    return rules->brand_count++;
}

static bool prefix_less(const RulePrefix* a, const RulePrefix* b) {
    if(a->oui != b->oui) return a->oui < b->oui;
    // First matching rule wins: lower brand index first
    return a->brand < b->brand;
}

static void sort_prefixes(Rules* rules) {
    // Insertion sort: a few hundred entries, loaded once
    for(uint16_t i = 1; i < rules->prefix_count; i++) {
        RulePrefix key = rules->prefixes[i];
        int j = i - 1;
        while(j >= 0 && prefix_less(&key, &rules->prefixes[j])) {
            rules->prefixes[j + 1] = rules->prefixes[j];
            j--;
        }
        rules->prefixes[j + 1] = key;
    }
}

static void parse_line(Rules* rules, char* line) {
    // Strip comments and whitespace
    char* hash = strchr(line, '#');
    if(hash) *hash = '\0';
    char* eq = strchr(line, '=');
    if(!eq) return;
    *eq = '\0';

    // Left side: "Brand" or "Brand [other]"
    char* left = line;
    bool other = false;
    char* tag = strcasestr(left, "[other]");
    if(tag) {
        other = true;
        *tag = '\0';
    }
    while(ch_is_space(*left))
        left++;
    char* end = left + strlen(left);
    while(end > left && ch_is_space(end[-1]))
        *--end = '\0';
    if(*left == '\0') return;

    uint8_t brand = find_or_add_brand(rules, left, other);
    if(brand == RULES_NO_MATCH) return;

    // Right side: "prefixes: ... names: ..."
    enum { None, Prefixes, Names } section = None;
    char* save = NULL;
    for(char* tok = strtok_r(eq + 1, " \t\r\n,", &save); tok;
        tok = strtok_r(NULL, " \t\r\n,", &save)) {
        // Section keywords: "prefixes:", "prefixes", ":" or "prefixes:286FB9"
        if(strncasecmp(tok, "prefixes", 8) == 0) {
            section = Prefixes;
            tok += 8;
        } else if(strncasecmp(tok, "names", 5) == 0 && section != Names) {
            section = Names;
            tok += 5;
        }
        if(*tok == ':') tok++;
        if(*tok == '\0') {
            continue;
        } else if(section == Prefixes) {
            uint32_t oui;
            if(parse_hex24(tok, &oui) && rules->prefix_count < RULES_MAX_PREFIXES) {
                rules->prefixes[rules->prefix_count].oui = oui;
                rules->prefixes[rules->prefix_count].brand = brand;
                rules->prefix_count++;
            }
        } else if(section == Names && rules->name_count < RULES_MAX_NAMES) {
            RuleName* n = &rules->names[rules->name_count++];
            strlcpy(n->pattern, tok, sizeof(n->pattern));
            str_lower(n->pattern);
            n->brand = brand;
        }
    }
}

RulesLoadResult rules_load(Rules* rules, const char* path) {
    memset(rules, 0, sizeof(Rules));

    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    FuriString* line = furi_string_alloc();
    RulesLoadResult result = RulesLoadMissing;

    if(file_stream_open(stream, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        while(stream_read_line(stream, line)) {
            size_t len = furi_string_size(line);
            char* buf = malloc(len + 1);
            memcpy(buf, furi_string_get_cstr(line), len + 1);
            parse_line(rules, buf);
            free(buf);
        }
        sort_prefixes(rules);
        result = rules->brand_count ? RulesLoadOk : RulesLoadEmpty;
    }

    furi_string_free(line);
    file_stream_close(stream);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
    return result;
}

uint8_t rules_match_prefix(const Rules* rules, uint32_t oui) {
    // Lower bound search so duplicates resolve to the lowest brand index
    size_t lo = 0, hi = rules->prefix_count;
    while(lo < hi) {
        size_t mid = (lo + hi) / 2;
        if(rules->prefixes[mid].oui < oui)
            lo = mid + 1;
        else
            hi = mid;
    }
    if(lo < rules->prefix_count && rules->prefixes[lo].oui == oui) {
        return rules->prefixes[lo].brand;
    }
    return RULES_NO_MATCH;
}

uint8_t rules_match_name(const Rules* rules, const char* name) {
    if(!name || !*name || rules->name_count == 0) return RULES_NO_MATCH;
    char lower[40];
    strlcpy(lower, name, sizeof(lower));
    str_lower(lower);
    uint8_t best = RULES_NO_MATCH;
    for(uint8_t i = 0; i < rules->name_count; i++) {
        if(rules->names[i].brand < best && strstr(lower, rules->names[i].pattern)) {
            best = rules->names[i].brand;
        }
    }
    memset(lower, 0, sizeof(lower)); // holds a network name
    return best;
}

bool rules_has_other(const Rules* rules) {
    for(uint8_t i = 0; i < rules->brand_count; i++) {
        if(rules->brands[i].other) return true;
    }
    return false;
}
