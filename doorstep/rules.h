#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define RULES_MAX_BRANDS   32
#define RULES_MAX_PREFIXES 512
#define RULES_MAX_NAMES    64
#define RULES_NAME_LEN     16
#define RULES_PATTERN_LEN  24

#define RULES_NO_MATCH 0xFF

typedef struct {
    char name[RULES_NAME_LEN];
    bool other; // rolled into the tally's "Other" line
} RuleBrand;

typedef struct {
    uint32_t oui; // 24-bit prefix
    uint8_t brand;
} RulePrefix;

typedef struct {
    char pattern[RULES_PATTERN_LEN]; // lowercase substring
    uint8_t brand;
} RuleName;

typedef struct {
    RuleBrand brands[RULES_MAX_BRANDS];
    uint8_t brand_count;
    RulePrefix prefixes[RULES_MAX_PREFIXES]; // sorted by oui
    uint16_t prefix_count;
    RuleName names[RULES_MAX_NAMES];
    uint8_t name_count;
} Rules;

typedef enum {
    RulesLoadOk,
    RulesLoadMissing,
    RulesLoadEmpty,
} RulesLoadResult;

/** Load rules from path. Repeated brand lines are merged. */
RulesLoadResult rules_load(Rules* rules, const char* path);

/** Brand index for a 24-bit prefix, or RULES_NO_MATCH. */
uint8_t rules_match_prefix(const Rules* rules, uint32_t oui);

/** Brand index for a network name (case-insensitive substring), or RULES_NO_MATCH. */
uint8_t rules_match_name(const Rules* rules, const char* name);

/** True if any brand is flagged [other]. */
bool rules_has_other(const Rules* rules);
