#pragma once

// Tiny ASCII helpers: the firmware API does not export libc's ctype functions.

static inline bool ch_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static inline bool ch_is_xdigit(char c) {
    return ch_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static inline bool ch_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static inline char ch_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static inline int ch_hexval(char c) {
    if(ch_is_digit(c)) return c - '0';
    c = ch_lower(c);
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
