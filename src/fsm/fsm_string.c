/* Text: the String category.
 *
 * Each operation is a single action, with the variations that other toolkits split into separate
 * actions — where to start searching, whether case matters, which end to trim — offered as parameters.
 *
 * State Graph strings hold up to LX_REF_MAX characters; results longer than that are cut off. */
#include "fsm_actions.h"

#define FSM_STR_CAP LX_REF_MAX

static void fsm_str_copy(char *dst, const char *src) { lx_strcpy(dst, FSM_STR_CAP, src ? src : ""); }

static int fsm_cmp(const char *a, const char *b, bool ignore_case) {
    if (!a) a = "";
    if (!b) b = "";
    return ignore_case ? fsm_stricmp(a, b) : strcmp(a, b);
}
/* index of `needle` in `haystack` at or after `from`, or -1 */
static int fsm_find(const char *haystack, const char *needle, int from, bool ignore_case) {
    if (!haystack || !needle || !needle[0]) return -1;
    int hl = (int)strlen(haystack), nl = (int)strlen(needle);
    /* clamp before the loop: a Start Index of INT_MAX would overflow `i + nl` and walk off the end */
    if (from < 0) from = 0;
    if (from > hl) return -1;
    for (int i = from; i <= hl - nl; i++) {
        int k = 0;
        while (k < nl) {
            char x = haystack[i + k], y = needle[k];
            if (ignore_case) { x = (char)tolower((unsigned char)x); y = (char)tolower((unsigned char)y); }
            if (x != y) break;
            k++;
        }
        if (k == nl) return i;
    }
    return -1;
}
static int fsm_find_last(const char *haystack, const char *needle, bool ignore_case) {
    int last = -1, at = 0;
    for (;;) {
        int i = fsm_find(haystack, needle, at, ignore_case);
        if (i < 0) break;
        last = i;
        at = i + 1;
    }
    return last;
}

/* ==========================================================================================
 *  basics
 * ========================================================================================== */
static void s_set(LxFsmCtx *c) { lx_fsm_set_string(c, 1, lx_fsm_string(c, 0)); }
static const LxFsmActionDesc A_S_SET = {"String Set Value", "String", "Stores a piece of text in a String variable.", "{1} = \"{0}\"",
    {P("Value", LX_FSM_STRING, ""), POUT("Store Result", LX_FSM_STRING)}, 0, 0, LX_FSM_ONCE, s_set, s_set, NULL};

static void s_append(LxFsmCtx *c) {
    char buf[FSM_STR_CAP];
    snprintf(buf, sizeof buf, "%s%s", lx_fsm_string(c, 0), lx_fsm_string(c, 1));
    lx_fsm_set_string(c, 0, buf);
}
static const LxFsmActionDesc A_S_APPEND = {"String Append", "String", "Adds text to the end of a String variable.", "{0} += \"{1}\"",
    {POUT("String Variable", LX_FSM_STRING), P("Append", LX_FSM_STRING, "")}, 0, 0, LX_FSM_ONCE, s_append, s_append, NULL};

static void s_concat(LxFsmCtx *c) {
    char buf[FSM_STR_CAP];
    snprintf(buf, sizeof buf, "%s%s", lx_fsm_string(c, 0), lx_fsm_string(c, 1));
    lx_fsm_set_string(c, 2, buf);
}
static const LxFsmActionDesc A_S_CONCAT = {"String Concat", "String", "Joins two pieces of text together.", "\"{0}\" + \"{1}\" -> {2}",
    {P("String 1", LX_FSM_STRING, ""), P("String 2", LX_FSM_STRING, ""), POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, s_concat, s_concat, NULL};

static void s_build(LxFsmCtx *c) {
    char buf[FSM_STR_CAP];
    const char *sep = lx_fsm_string(c, 5);
    size_t at = 0;
    buf[0] = 0;
    for (int i = 0; i < 5; i++) {
        if (lx_fsm_is_none(c, i)) continue;
        const char *part = lx_fsm_string(c, i);
        if (at > 0 && sep && sep[0]) at += (size_t)snprintf(buf + at, sizeof buf - at, "%s", sep);
        if (at >= sizeof buf - 1) break;
        at += (size_t)snprintf(buf + at, sizeof buf - at, "%s", part ? part : "");
        if (at >= sizeof buf - 1) break;
    }
    lx_fsm_set_string(c, 6, buf);
}
static const LxFsmActionDesc A_S_BUILD = {"Build String", "String", "Joins up to five pieces of text, with an optional separator between them.",
    "build -> {6}",
    {PIN_OPT("String 1", LX_FSM_STRING), PIN_OPT("String 2", LX_FSM_STRING), PIN_OPT("String 3", LX_FSM_STRING),
     PIN_OPT("String 4", LX_FSM_STRING), PIN_OPT("String 5", LX_FSM_STRING),
     PT("Separator", LX_FSM_STRING, "", "put between each part (\", \" for a list)"), POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, s_build, s_build, NULL};

/* Format: {0}..{3} in the template are replaced by the four values, each of any type. */
static void s_format(LxFsmCtx *c) {
    const char *fmt = lx_fsm_string(c, 0);
    char args[4][FSM_STR_CAP];
    for (int i = 0; i < 4; i++) {
        args[i][0] = 0;
        if (lx_fsm_is_none(c, 1 + i)) continue;
        LxFsmValue *v = lx_fsm_param_value(c, 1 + i);
        if (v) lx_fsm_value_format(v, args[i], sizeof args[i], NULL);
    }
    char out[FSM_STR_CAP];
    size_t at = 0;
    for (const char *p = fmt ? fmt : ""; *p && at < sizeof out - 1; p++) {
        if (p[0] == '{' && p[1] >= '0' && p[1] <= '3' && p[2] == '}') {
            at += (size_t)snprintf(out + at, sizeof out - at, "%s", args[p[1] - '0']);
            p += 2;
        } else {
            out[at++] = *p;
        }
    }
    out[at < sizeof out ? at : sizeof out - 1] = 0;
    lx_fsm_set_string(c, 5, out);
}
static const LxFsmActionDesc A_S_FORMAT = {"Format String", "String", "Fills {0}, {1}, {2} and {3} in a template with values of any type: \"Score: {0}\".",
    "format \"{0}\" -> {5}",
    {PT("Format", LX_FSM_STRING, "Score: {0}", "{0}..{3} are replaced by the values below"),
     PIN_OPT("Value 0", LX_FSM_ANY), PIN_OPT("Value 1", LX_FSM_ANY), PIN_OPT("Value 2", LX_FSM_ANY), PIN_OPT("Value 3", LX_FSM_ANY),
     POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, s_format, s_format, NULL};

static void s_length(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0);
    lx_fsm_set_int(c, 1, (int)strlen(s ? s : ""));
}
static const LxFsmActionDesc A_S_LENGTH = {"String Length", "String", "How many characters a piece of text has.", "len \"{0}\" -> {1}",
    {P("String", LX_FSM_STRING, ""), POUT("Store Result", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, s_length, s_length, NULL};

static void s_is_empty(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0);
    bool empty = !s || !s[0];
    if (!empty && lx_fsm_bool(c, 1)) {
        empty = true;
        for (const char *p = s; *p; p++) if (!isspace((unsigned char)*p)) { empty = false; break; }
    }
    lx_fsm_set_bool(c, 2, empty);
    fsm_send(c, empty ? 3 : 4);
}
static const LxFsmActionDesc A_S_IS_EMPTY = {"String Is Empty", "String", "True when a piece of text has nothing in it.", "\"{0}\" empty?",
    {P("String", LX_FSM_STRING, ""), PT("Or Whitespace", LX_FSM_BOOL, "true", "spaces and tabs count as empty too"),
     POPT("Store Result", LX_FSM_BOOL), PEV("Empty Event"), PEV("Not Empty Event")},
    0, 0, LX_FSM_ONCE, s_is_empty, s_is_empty, NULL};

/* ==========================================================================================
 *  comparing and searching
 * ========================================================================================== */
static void s_equals(LxFsmCtx *c) {
    bool eq = fsm_cmp(lx_fsm_string(c, 0), lx_fsm_string(c, 1), lx_fsm_bool(c, 2)) == 0;
    lx_fsm_set_bool(c, 3, eq);
    fsm_send(c, eq ? 4 : 5);
}
static const LxFsmActionDesc A_S_EQUALS = {"String Equals", "String", "Tests whether two pieces of text are the same.", "\"{0}\" == \"{1}\"?",
    {P("String 1", LX_FSM_STRING, ""), P("String 2", LX_FSM_STRING, ""), P("Ignore Case", LX_FSM_BOOL, "false"),
     POPT("Store Result", LX_FSM_BOOL), PEV("Equal Event"), PEV("Not Equal Event")},
    0, 0, LX_FSM_ONCE, s_equals, s_equals, NULL};

static void s_compare(LxFsmCtx *c) {
    int r = fsm_cmp(lx_fsm_string(c, 0), lx_fsm_string(c, 1), lx_fsm_bool(c, 2));
    lx_fsm_set_int(c, 3, r < 0 ? -1 : (r > 0 ? 1 : 0));
    if (r == 0) fsm_send(c, 4);
    else if (r < 0) fsm_send(c, 5);
    else fsm_send(c, 6);
}
static const LxFsmActionDesc A_S_COMPARE = {"String Compare", "String", "Compares two pieces of text alphabetically and sends an event for each outcome.",
    "compare \"{0}\" to \"{1}\"",
    {P("String 1", LX_FSM_STRING, ""), P("String 2", LX_FSM_STRING, ""), P("Ignore Case", LX_FSM_BOOL, "false"),
     POPT("Store Result", LX_FSM_INT), PEV("Equal Event"), PEV("Less Event"), PEV("Greater Event")},
    0, 0, LX_FSM_ONCE, s_compare, s_compare, NULL};

static void s_contains(LxFsmCtx *c) {
    bool has = fsm_find(lx_fsm_string(c, 0), lx_fsm_string(c, 1), 0, lx_fsm_bool(c, 2)) >= 0;
    lx_fsm_set_bool(c, 3, has);
    fsm_send(c, has ? 4 : 5);
}
static const LxFsmActionDesc A_S_CONTAINS = {"String Contains", "String", "True when a piece of text contains another somewhere inside it.",
    "\"{0}\" contains \"{1}\"?",
    {P("String", LX_FSM_STRING, ""), P("Contains", LX_FSM_STRING, ""), P("Ignore Case", LX_FSM_BOOL, "false"),
     POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_ONCE, s_contains, s_contains, NULL};

static void s_starts_ends(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0), *part = lx_fsm_string(c, 1);
    bool ignore = lx_fsm_bool(c, 3);
    int sl = (int)strlen(s ? s : ""), pl = (int)strlen(part ? part : "");
    bool r = false;
    if (pl > 0 && pl <= sl) {
        int at = lx_fsm_int(c, 2) == 0 ? 0 : sl - pl;
        r = fsm_find(s, part, at, ignore) == at;
    }
    lx_fsm_set_bool(c, 4, r);
    fsm_send(c, r ? 5 : 6);
}
static const LxFsmActionDesc A_S_STARTS_ENDS = {"String Starts Or Ends With", "String", "True when a piece of text begins or ends with another.",
    "\"{0}\" {2} \"{1}\"?",
    {P("String", LX_FSM_STRING, ""), P("Part", LX_FSM_STRING, ""), PENUM("Test", "Starts With|Ends With", "0"),
     P("Ignore Case", LX_FSM_BOOL, "false"), POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_ONCE, s_starts_ends, s_starts_ends, NULL};

static void s_index_of(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0), *part = lx_fsm_string(c, 1);
    bool ignore = lx_fsm_bool(c, 3);
    int at = lx_fsm_int(c, 2) == 0 ? fsm_find(s, part, lx_fsm_int(c, 4), ignore) : fsm_find_last(s, part, ignore);
    lx_fsm_set_int(c, 5, at);
    fsm_send(c, at >= 0 ? 6 : 7);
}
static const LxFsmActionDesc A_S_INDEX_OF = {"String Index Of", "String", "Where one piece of text appears inside another; -1 when it does not.",
    "find \"{1}\" in \"{0}\" -> {5}",
    {P("String", LX_FSM_STRING, ""), P("Find", LX_FSM_STRING, ""), PENUM("Search", "First|Last", "0"),
     P("Ignore Case", LX_FSM_BOOL, "false"), PT("Start Index", LX_FSM_INT, "0", "where to start looking (First only)"),
     POUT("Store Result", LX_FSM_INT), PEV("Found Event"), PEV("Not Found Event")},
    0, 0, LX_FSM_ONCE, s_index_of, s_index_of, NULL};

/* ==========================================================================================
 *  changing text
 * ========================================================================================== */
static void s_substring(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0);
    int len = (int)strlen(s ? s : "");
    int start = fsm_index(lx_fsm_int(c, 1), len);   /* a negative start counts back from the end */
    if (start < 0) start = 0;
    int count = fsm_count_from(start, lx_fsm_int(c, 2), len);
    char buf[FSM_STR_CAP];
    int n = count < (int)sizeof buf - 1 ? count : (int)sizeof buf - 1;
    memcpy(buf, s + start, (size_t)n);
    buf[n] = 0;
    lx_fsm_set_string(c, 3, buf);
}
static const LxFsmActionDesc A_S_SUBSTRING = {"String Substring", "String", "Takes a piece out of some text. A negative start counts back from the end; Length 0 means \"to the end\".",
    "\"{0}\"[{1}..] -> {3}",
    {P("String", LX_FSM_STRING, ""), P("Start Index", LX_FSM_INT, "0"), PT("Length", LX_FSM_INT, "0", "0 = to the end"),
     POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, s_substring, s_substring, NULL};

static void s_replace(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0), *find = lx_fsm_string(c, 1), *with = lx_fsm_string(c, 2);
    bool ignore = lx_fsm_bool(c, 3);
    char out[FSM_STR_CAP];
    size_t at = 0;
    int i = 0, fl = (int)strlen(find ? find : ""), count = 0;
    int sl = (int)strlen(s ? s : "");
    if (fl == 0) { lx_fsm_set_string(c, 4, s); lx_fsm_set_int(c, 5, 0); return; }
    while (i < sl && at < sizeof out - 1) {
        int hit = fsm_find(s, find, i, ignore);
        if (hit < 0) break;
        int run = hit - i;
        if (run > 0) {
            size_t room = sizeof out - 1 - at;
            size_t n = (size_t)run < room ? (size_t)run : room;
            memcpy(out + at, s + i, n);
            at += n;
        }
        at += (size_t)snprintf(out + at, sizeof out - at, "%s", with ? with : "");
        i = hit + fl;
        count++;
    }
    if (i < sl && at < sizeof out - 1) snprintf(out + at, sizeof out - at, "%s", s + i);
    else out[at < sizeof out ? at : sizeof out - 1] = 0;
    lx_fsm_set_string(c, 4, out);
    lx_fsm_set_int(c, 5, count);
}
static const LxFsmActionDesc A_S_REPLACE = {"String Replace", "String", "Replaces every occurrence of one piece of text with another.",
    "replace \"{1}\" with \"{2}\" -> {4}",
    {P("String", LX_FSM_STRING, ""), P("Find", LX_FSM_STRING, ""), P("Replace With", LX_FSM_STRING, ""),
     P("Ignore Case", LX_FSM_BOOL, "false"), POUT("Store Result", LX_FSM_STRING), POPT("Store Count", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, s_replace, s_replace, NULL};

static void s_insert(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0), *add = lx_fsm_string(c, 2);
    int len = (int)strlen(s ? s : "");
    int at = lx_fsm_int(c, 1);
    at = at < 0 ? 0 : (at > len ? len : at);
    char buf[FSM_STR_CAP];
    snprintf(buf, sizeof buf, "%.*s%s%s", at, s, add ? add : "", s + at);
    lx_fsm_set_string(c, 3, buf);
}
static const LxFsmActionDesc A_S_INSERT = {"String Insert", "String", "Puts a piece of text into another at a position.", "insert \"{2}\" at {1} -> {3}",
    {P("String", LX_FSM_STRING, ""), P("Index", LX_FSM_INT, "0"), P("Insert", LX_FSM_STRING, ""), POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, s_insert, s_insert, NULL};

static void s_remove(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0);
    int len = (int)strlen(s ? s : "");
    int start = fsm_index(lx_fsm_int(c, 1), len);
    if (start < 0) start = 0;
    int count = fsm_count_from(start, lx_fsm_int(c, 2), len);
    char buf[FSM_STR_CAP];
    snprintf(buf, sizeof buf, "%.*s%s", start, s, s + start + count);
    lx_fsm_set_string(c, 3, buf);
}
static const LxFsmActionDesc A_S_REMOVE = {"String Remove", "String", "Cuts a run of characters out of a piece of text.", "remove {2} at {1} -> {3}",
    {P("String", LX_FSM_STRING, ""), P("Start Index", LX_FSM_INT, "0"), PT("Count", LX_FSM_INT, "0", "0 = to the end"),
     POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, s_remove, s_remove, NULL};

static void s_case(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0);
    int mode = lx_fsm_int(c, 1);
    char buf[FSM_STR_CAP];
    fsm_str_copy(buf, s);
    bool word_start = true;
    for (char *p = buf; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        switch (mode) {
        case 0: *p = (char)tolower(ch); break;
        case 1: *p = (char)toupper(ch); break;
        default:
            *p = word_start ? (char)toupper(ch) : (char)tolower(ch);
            word_start = !isalnum(ch);
            break;
        }
    }
    lx_fsm_set_string(c, 2, buf);
}
static const LxFsmActionDesc A_S_CASE = {"String Change Case", "String", "Makes a piece of text lower case, UPPER CASE or Title Case.", "{1} \"{0}\" -> {2}",
    {P("String", LX_FSM_STRING, ""), PENUM("Case", "Lower|Upper|Title", "0"), POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, s_case, s_case, NULL};

static void s_trim(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0);
    const char *cut = lx_fsm_string(c, 2);
    bool use_set = cut && cut[0];
    int mode = lx_fsm_int(c, 1);
    const char *b = s ? s : "";
    const char *e = b + strlen(b);
    #define FSM_TRIMMABLE(ch) (use_set ? strchr(cut, (ch)) != NULL : isspace((unsigned char)(ch)) != 0)
    if (mode != 2) while (b < e && FSM_TRIMMABLE(*b)) b++;          /* Both or Start */
    if (mode != 1) while (e > b && FSM_TRIMMABLE(e[-1])) e--;       /* Both or End */
    #undef FSM_TRIMMABLE
    char buf[FSM_STR_CAP];
    int n = (int)(e - b);
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    memcpy(buf, b, (size_t)n);
    buf[n] = 0;
    lx_fsm_set_string(c, 3, buf);
}
static const LxFsmActionDesc A_S_TRIM = {"String Trim", "String", "Removes spaces (or characters of your choosing) from the start and end of a piece of text.",
    "trim \"{0}\" -> {3}",
    {P("String", LX_FSM_STRING, ""), PENUM("Trim", "Both|Start|End", "0"),
     PT("Characters", LX_FSM_STRING, "", "which characters to remove; empty = whitespace"), POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, s_trim, s_trim, NULL};

static void s_pad(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0);
    int want = lx_fsm_int(c, 1);
    const char *padc = lx_fsm_string(c, 3);
    char fill = (padc && padc[0]) ? padc[0] : ' ';
    int len = (int)strlen(s ? s : "");
    char buf[FSM_STR_CAP];
    if (want <= len || want >= (int)sizeof buf) { fsm_str_copy(buf, s); }
    else {
        int pad = want - len;
        if (lx_fsm_int(c, 2) == 0) {                 /* pad on the left */
            for (int i = 0; i < pad; i++) buf[i] = fill;
            lx_strcpy(buf + pad, sizeof buf - (size_t)pad, s ? s : "");
        } else {
            fsm_str_copy(buf, s);
            for (int i = 0; i < pad; i++) buf[len + i] = fill;
            buf[want] = 0;
        }
    }
    lx_fsm_set_string(c, 4, buf);
}
static const LxFsmActionDesc A_S_PAD = {"String Pad", "String", "Makes a piece of text a fixed width by adding characters at the start or end (\"007\", aligned columns).",
    "pad \"{0}\" to {1} -> {4}",
    {P("String", LX_FSM_STRING, ""), P("Total Length", LX_FSM_INT, "8"), PENUM("Side", "Left|Right", "0"),
     PT("Pad Character", LX_FSM_STRING, " ", "the first character of this is used"), POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, s_pad, s_pad, NULL};

static void s_select(LxFsmCtx *c) { lx_fsm_set_string(c, 3, lx_fsm_bool(c, 0) ? lx_fsm_string(c, 1) : lx_fsm_string(c, 2)); }
static const LxFsmActionDesc A_S_SELECT = {"String Select Value", "String", "Picks one of two pieces of text depending on a bool.", "{3} = {0} ? \"{1}\" : \"{2}\"",
    {P("Condition", LX_FSM_BOOL, "true"), P("If True", LX_FSM_STRING, ""), P("If False", LX_FSM_STRING, ""), POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, s_select, s_select, NULL};

static void s_char_at(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0);
    int len = (int)strlen(s ? s : "");
    int i = fsm_index(lx_fsm_int(c, 1), len);
    char buf[2] = {0, 0};
    if (i >= 0 && i < len) buf[0] = s[i];
    lx_fsm_set_string(c, 2, buf);
    lx_fsm_set_int(c, 3, buf[0] ? (int)(unsigned char)buf[0] : -1);
}
static const LxFsmActionDesc A_S_CHAR_AT = {"String Get Char", "String", "The single character at a position (a negative index counts back from the end).",
    "\"{0}\"[{1}] -> {2}",
    {P("String", LX_FSM_STRING, ""), P("Index", LX_FSM_INT, "0"), POUT("Store Char", LX_FSM_STRING), POPT("Store Code", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, s_char_at, s_char_at, NULL};

/* ==========================================================================================
 *  splitting and joining with arrays
 * ========================================================================================== */
static void s_split(LxFsmCtx *c) {
    LxArray out = lx_fsm_array(c, 2);
    if (!lx_array_valid(out)) return;
    lx_array_clear(out);
    const char *s = lx_fsm_string(c, 0);
    const char *sep = lx_fsm_string(c, 1);
    bool skip_empty = lx_fsm_bool(c, 3);
    if (!s) s = "";
    if (!sep || !sep[0]) { lx_array_add_string(out, s); lx_fsm_set_int(c, 4, 1); return; }
    int sl = (int)strlen(sep), at = 0, n = 0;
    for (;;) {
        int hit = fsm_find(s, sep, at, false);
        int end = hit < 0 ? (int)strlen(s) : hit;
        char piece[FSM_STR_CAP];
        int len = end - at;
        if (len > (int)sizeof piece - 1) len = (int)sizeof piece - 1;
        memcpy(piece, s + at, (size_t)len);
        piece[len] = 0;
        if (!(skip_empty && len == 0)) { lx_array_add_string(out, piece); n++; }
        if (hit < 0) break;
        at = hit + sl;
    }
    lx_fsm_set_int(c, 4, n);
}
static const LxFsmActionDesc A_S_SPLIT = {"String Split", "String", "Breaks a piece of text apart at a separator and puts the parts in an array.",
    "split \"{0}\" by \"{1}\" -> {2}",
    {P("String", LX_FSM_STRING, ""), P("Separator", LX_FSM_STRING, ","), PARRT("Store Array", "a String array"),
     PT("Skip Empty", LX_FSM_BOOL, "false", "leave out blank parts"), POPT("Store Count", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, s_split, s_split, NULL};

static void s_join(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    const char *sep = lx_fsm_string(c, 1);
    char out[FSM_STR_CAP];
    size_t at = 0;
    out[0] = 0;
    int n = lx_array_valid(a) ? lx_array_length(a) : 0;
    for (int i = 0; i < n && at < sizeof out - 1; i++) {
        if (i > 0 && sep && sep[0]) at += (size_t)snprintf(out + at, sizeof out - at, "%s", sep);
        if (at >= sizeof out - 1) break;
        at += (size_t)snprintf(out + at, sizeof out - at, "%s", lx_array_get_string(a, i));
    }
    lx_fsm_set_string(c, 2, out);
}
static const LxFsmActionDesc A_S_JOIN = {"String Join", "String", "Joins every item of an array into one piece of text, with a separator between them.",
    "join {0} -> {2}",
    {PARR("Array"), P("Separator", LX_FSM_STRING, ", "), POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, s_join, s_join, NULL};

/* ==========================================================================================
 *  typewriter — reveals text one character at a time
 * ========================================================================================== */
typedef struct { float t; int shown; } FsmTypewriter;
static void s_typewriter_enter(LxFsmCtx *c) {
    FsmTypewriter *w = c->state;
    w->t = 0;
    w->shown = 0;
}
static void s_typewriter_update(LxFsmCtx *c) {
    FsmTypewriter *w = c->state;
    const char *s = lx_fsm_string(c, 0);
    int len = (int)strlen(s ? s : "");
    float cps = lx_maxf(0.0001f, lx_fsm_float(c, 1));
    w->t += c->dt;
    int want = (int)(w->t * cps);
    if (want > len) want = len;
    if (want != w->shown) {
        w->shown = want;
        char buf[FSM_STR_CAP];
        int n = want < (int)sizeof buf - 1 ? want : (int)sizeof buf - 1;
        memcpy(buf, s, (size_t)n);
        buf[n] = 0;
        lx_fsm_set_string(c, 2, buf);
    }
    lx_fsm_set_float(c, 3, len > 0 ? (float)w->shown / (float)len : 1.0f);
    if (w->shown >= len) { lx_fsm_finish(c); fsm_send(c, 4); }
}
static const LxFsmActionDesc A_S_TYPEWRITER = {"String Typewriter", "String", "Reveals a piece of text one character at a time, as dialogue boxes do.",
    "type \"{0}\" at {1}/s",
    {P("String", LX_FSM_STRING, ""), P("Chars Per Second", LX_FSM_FLOAT, "30"), POUT("Store Result", LX_FSM_STRING),
     POPT("Store Progress", LX_FSM_FLOAT), PEV("Finish Event")},
    sizeof(FsmTypewriter), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, s_typewriter_enter, s_typewriter_update, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_string(void) {
    static const LxFsmActionDesc *all[] = {
        &A_S_SET, &A_S_APPEND, &A_S_CONCAT, &A_S_BUILD, &A_S_FORMAT, &A_S_LENGTH, &A_S_IS_EMPTY,
        &A_S_EQUALS, &A_S_COMPARE, &A_S_CONTAINS, &A_S_STARTS_ENDS, &A_S_INDEX_OF,
        &A_S_SUBSTRING, &A_S_REPLACE, &A_S_INSERT, &A_S_REMOVE, &A_S_CASE, &A_S_TRIM, &A_S_PAD,
        &A_S_SELECT, &A_S_CHAR_AT, &A_S_SPLIT, &A_S_JOIN, &A_S_TYPEWRITER,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
