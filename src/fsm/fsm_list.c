/* Lists: the List category.
 *
 * A List is an Array variable: a run of values all of one type. Items are numbered from 0; a negative
 * index counts back from the end, so -1 is the last item. Reading past the end gives 0 / "" / None and
 * writing past the end does nothing, so a graph cannot crash on a bad index.
 *
 * Items are read and written through LX_FSM_ANY parameters, so one action works for every element type. */
#include "fsm_actions.h"

static LxArray fsm_list(LxFsmCtx *c, int p) { return lx_fsm_array(c, p); }

/* ==========================================================================================
 *  size
 * ========================================================================================== */
static void l_count(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    int n = lx_array_valid(a) ? lx_array_length(a) : 0;
    lx_fsm_set_int(c, 1, n);
    lx_fsm_set_bool(c, 2, n == 0);
    fsm_send(c, n == 0 ? 3 : 4);
}
static const LxFsmActionDesc A_L_COUNT = {"List Count", "List", "How many items a list holds, and whether it is empty.", "count {0} -> {1}",
    {PARR("List"), POUT("Store Count", LX_FSM_INT), POPT("Store Is Empty", LX_FSM_BOOL),
     PEV("Empty Event"), PEV("Not Empty Event")},
    0, 0, LX_FSM_ONCE, l_count, l_count, NULL};

static void l_clear(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    if (lx_array_valid(a)) lx_array_clear(a);
}
static const LxFsmActionDesc A_L_CLEAR = {"List Clear", "List", "Removes every item from a list.", "clear {0}",
    {PARR("List")}, 0, 0, LX_FSM_ONCE, l_clear, l_clear, NULL};

static void l_resize(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    if (lx_array_valid(a)) lx_array_resize(a, lx_fsm_int(c, 1) < 0 ? 0 : lx_fsm_int(c, 1));
}
static const LxFsmActionDesc A_L_RESIZE = {"List Resize", "List", "Makes a list exactly this long, adding empty items or cutting the end off.",
    "resize {0} to {1}",
    {PARR("List"), P("Length", LX_FSM_INT, "0")}, 0, 0, LX_FSM_ONCE, l_resize, l_resize, NULL};

/* ==========================================================================================
 *  adding and removing
 * ========================================================================================== */
static void l_add(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    if (!lx_array_valid(a)) return;
    int at = lx_array_length(a);
    lx_array_resize(a, at + 1);
    lx_fsm_array_set_from(c, 1, a, at);
    lx_fsm_set_int(c, 2, at);
}
static const LxFsmActionDesc A_L_ADD = {"List Add", "List", "Adds an item to the end of a list.", "add {1} to {0}",
    {PARR("List"), P("Value", LX_FSM_ANY, ""), POPT("Store Index", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, l_add, l_add, NULL};

static void l_add_unique(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    if (!lx_array_valid(a)) return;
    /* compare as text so one action covers every element type */
    LxFsmValue *v = lx_fsm_param_value(c, 1);
    char want[LX_REF_MAX];
    want[0] = 0;
    if (v) lx_fsm_value_format(v, want, sizeof want, NULL);
    int n = lx_array_length(a);
    for (int i = 0; i < n; i++) {
        if (!strcmp(lx_array_get_string(a, i), want)) {
            lx_fsm_set_int(c, 2, i);
            lx_fsm_set_bool(c, 3, false);
            fsm_send(c, 5);
            return;
        }
    }
    lx_array_resize(a, n + 1);
    lx_fsm_array_set_from(c, 1, a, n);
    lx_fsm_set_int(c, 2, n);
    lx_fsm_set_bool(c, 3, true);
    fsm_send(c, 4);
}
static const LxFsmActionDesc A_L_ADD_UNIQUE = {"List Add Unique", "List", "Adds an item only when the list does not already hold it.", "add {1} to {0} if new",
    {PARR("List"), P("Value", LX_FSM_ANY, ""), POPT("Store Index", LX_FSM_INT), POPT("Store Added", LX_FSM_BOOL),
     PEV("Added Event"), PEV("Already Present Event")},
    0, 0, LX_FSM_ONCE, l_add_unique, l_add_unique, NULL};

static void l_insert(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    if (!lx_array_valid(a)) return;
    int n = lx_array_length(a);
    int i = fsm_index(lx_fsm_int(c, 1), n);
    if (i < 0) i = 0;
    lx_array_insert_at(a, i);
    lx_fsm_array_set_from(c, 2, a, i);
}
static const LxFsmActionDesc A_L_INSERT = {"List Insert", "List", "Puts an item into a list at a position, pushing the rest along.", "insert {2} into {0} at {1}",
    {PARR("List"), P("Index", LX_FSM_INT, "0"), P("Value", LX_FSM_ANY, "")},
    0, 0, LX_FSM_ONCE, l_insert, l_insert, NULL};

static void l_remove_at(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    int i;
    if (!fsm_array_index(a, lx_fsm_int(c, 1), &i)) { fsm_send(c, 3); return; }
    lx_fsm_set_from_array(c, 2, a, i);
    lx_array_remove_at(a, i);
}
static const LxFsmActionDesc A_L_REMOVE_AT = {"List Remove At", "List", "Takes the item at a position out of a list, and hands it to you.",
    "remove {0}[{1}]",
    {PARR("List"), P("Index", LX_FSM_INT, "0"), POPT("Store Removed", LX_FSM_ANY), PEV("Out Of Range Event")},
    0, 0, LX_FSM_ONCE, l_remove_at, l_remove_at, NULL};

static void l_remove_value(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    if (!lx_array_valid(a)) return;
    LxFsmValue *v = lx_fsm_param_value(c, 1);
    char want[LX_REF_MAX];
    want[0] = 0;
    if (v) lx_fsm_value_format(v, want, sizeof want, NULL);
    bool all = lx_fsm_bool(c, 2);
    int removed = 0;
    for (int i = lx_array_length(a) - 1; i >= 0; i--) {
        if (strcmp(lx_array_get_string(a, i), want)) continue;
        lx_array_remove_at(a, i);
        removed++;
        if (!all) break;
    }
    lx_fsm_set_int(c, 3, removed);
    fsm_send(c, removed > 0 ? 4 : 5);
}
static const LxFsmActionDesc A_L_REMOVE_VALUE = {"List Remove Value", "List", "Takes an item out of a list by its value, rather than its position.",
    "remove {1} from {0}",
    {PARR("List"), P("Value", LX_FSM_ANY, ""), PT("All Matches", LX_FSM_BOOL, "false", "remove every copy, not just the first"),
     POPT("Store Removed Count", LX_FSM_INT), PEV("Removed Event"), PEV("Not Found Event")},
    0, 0, LX_FSM_ONCE, l_remove_value, l_remove_value, NULL};

static void l_remove_first_last(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    if (!lx_array_valid(a) || lx_array_length(a) == 0) { fsm_send(c, 3); return; }
    int i = lx_fsm_int(c, 1) == 0 ? 0 : lx_array_length(a) - 1;
    lx_fsm_set_from_array(c, 2, a, i);
    lx_array_remove_at(a, i);
}
static const LxFsmActionDesc A_L_POP = {"List Pop", "List", "Takes the first or last item off a list and hands it to you: queues and stacks.",
    "pop {1} of {0} -> {2}",
    {PARR("List"), PENUM("From", "First|Last", "1"), POPT("Store Value", LX_FSM_ANY), PEV("Empty Event")},
    0, 0, LX_FSM_ONCE, l_remove_first_last, l_remove_first_last, NULL};

/* ==========================================================================================
 *  reading and writing items
 * ========================================================================================== */
static void l_get(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    int i;
    if (!fsm_array_index(a, lx_fsm_int(c, 1), &i)) { fsm_send(c, 3); return; }
    lx_fsm_set_from_array(c, 2, a, i);
    fsm_send(c, 4);
}
static const LxFsmActionDesc A_L_GET = {"List Get", "List", "Reads the item at a position. A negative index counts back from the end.",
    "{0}[{1}] -> {2}",
    {PARR("List"), P("Index", LX_FSM_INT, "0"), POUT("Store Value", LX_FSM_ANY), PEV("Out Of Range Event"), PEV("Found Event")},
    0, 0, LX_FSM_ONCE, l_get, l_get, NULL};

static void l_set(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    int i;
    if (!fsm_array_index(a, lx_fsm_int(c, 1), &i)) { fsm_send(c, 3); return; }
    lx_fsm_array_set_from(c, 2, a, i);
}
static const LxFsmActionDesc A_L_SET = {"List Set", "List", "Replaces the item at a position.", "{0}[{1}] = {2}",
    {PARR("List"), P("Index", LX_FSM_INT, "0"), P("Value", LX_FSM_ANY, ""), PEV("Out Of Range Event")},
    0, 0, LX_FSM_ONCE, l_set, l_set, NULL};

static void l_first_last(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    int n = lx_array_valid(a) ? lx_array_length(a) : 0;
    if (n == 0) { fsm_send(c, 3); return; }
    lx_fsm_set_from_array(c, 1, a, 0);
    lx_fsm_set_from_array(c, 2, a, n - 1);
}
static const LxFsmActionDesc A_L_FIRST_LAST = {"List Get First Last", "List", "The first and last items of a list.", "ends of {0}",
    {PARR("List"), POPT("Store First", LX_FSM_ANY), POPT("Store Last", LX_FSM_ANY), PEV("Empty Event")},
    0, 0, LX_FSM_ONCE, l_first_last, l_first_last, NULL};

static void l_random(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    int n = lx_array_valid(a) ? lx_array_length(a) : 0;
    if (n == 0) { fsm_send(c, 3); return; }
    int i = lx_random_int(0, n - 1);
    lx_fsm_set_from_array(c, 1, a, i);
    lx_fsm_set_int(c, 2, i);
}
static const LxFsmActionDesc A_L_RANDOM = {"List Get Random", "List", "Picks a random item out of a list: random spawn points, random lines of dialogue.",
    "random of {0} -> {1}",
    {PARR("List"), POUT("Store Value", LX_FSM_ANY), POPT("Store Index", LX_FSM_INT), PEV("Empty Event")},
    0, 0, LX_FSM_ONCE, l_random, l_random, NULL};

/* Steps through a list one item per run: the usual way to walk a list from a single state. */
typedef struct { int next; bool started; } FsmListWalk;
static void l_next(LxFsmCtx *c) {
    FsmListWalk *w = c->state;
    LxArray a = fsm_list(c, 0);
    int n = lx_array_valid(a) ? lx_array_length(a) : 0;
    if (n == 0) { fsm_send(c, 5); return; }
    if (!w->started) { w->started = true; w->next = 0; }
    if (w->next >= n) {
        if (!lx_fsm_bool(c, 1)) { fsm_send(c, 5); return; }   /* not looping: done */
        w->next = 0;
    }
    int i = w->next++;
    lx_fsm_set_from_array(c, 2, a, i);
    lx_fsm_set_int(c, 3, i);
    lx_fsm_set_bool(c, 4, i == n - 1);
    fsm_send(c, i == n - 1 && !lx_fsm_bool(c, 1) ? 6 : 7);
}
static const LxFsmActionDesc A_L_NEXT = {"List Get Next", "List", "Hands you the next item each time it runs, walking through the whole list. Re-entering the state starts again.",
    "next of {0} -> {2}",
    {PARR("List"), PT("Loop", LX_FSM_BOOL, "false", "start again at the beginning instead of finishing"),
     POUT("Store Value", LX_FSM_ANY), POPT("Store Index", LX_FSM_INT), POPT("Store Is Last", LX_FSM_BOOL),
     PEV("Finished Event"), PEV("Last Item Event"), PEV("Next Event")},
    sizeof(FsmListWalk), 0, LX_FSM_ONCE, l_next, l_next, NULL};

/* ==========================================================================================
 *  searching
 * ========================================================================================== */
static void l_index_of(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    if (!lx_array_valid(a)) { lx_fsm_set_int(c, 2, -1); fsm_send(c, 4); return; }
    LxFsmValue *v = lx_fsm_param_value(c, 1);
    char want[LX_REF_MAX];
    want[0] = 0;
    if (v) lx_fsm_value_format(v, want, sizeof want, NULL);
    int n = lx_array_length(a), found = -1, count = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(lx_array_get_string(a, i), want)) continue;
        if (found < 0) found = i;
        count++;
    }
    lx_fsm_set_int(c, 2, found);
    lx_fsm_set_int(c, 3, count);
    fsm_send(c, found >= 0 ? 5 : 4);
}
static const LxFsmActionDesc A_L_INDEX_OF = {"List Index Of", "List", "Where a value sits in a list (-1 when it is not there), and how many copies there are.",
    "find {1} in {0} -> {2}",
    {PARR("List"), P("Value", LX_FSM_ANY, ""), POUT("Store Index", LX_FSM_INT), POPT("Store Count", LX_FSM_INT),
     PEV("Not Found Event"), PEV("Found Event")},
    0, 0, LX_FSM_ONCE, l_index_of, l_index_of, NULL};

static void l_contains(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    bool has = false;
    if (lx_array_valid(a)) {
        LxFsmValue *v = lx_fsm_param_value(c, 1);
        char want[LX_REF_MAX];
        want[0] = 0;
        if (v) lx_fsm_value_format(v, want, sizeof want, NULL);
        int n = lx_array_length(a);
        for (int i = 0; i < n && !has; i++) has = !strcmp(lx_array_get_string(a, i), want);
    }
    lx_fsm_set_bool(c, 2, has);
    fsm_send(c, has ? 3 : 4);
}
static const LxFsmActionDesc A_L_CONTAINS = {"List Contains", "List", "True when a list holds a value.", "{0} contains {1}?",
    {PARR("List"), P("Value", LX_FSM_ANY, ""), POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_ONCE, l_contains, l_contains, NULL};

/* ==========================================================================================
 *  ordering
 * ========================================================================================== */
static void l_sort(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    if (lx_array_valid(a)) lx_array_sort(a, lx_fsm_int(c, 1) == 1);
}
static const LxFsmActionDesc A_L_SORT = {"List Sort", "List", "Puts a list in order: numbers by value, text alphabetically, objects by name.",
    "sort {0} {1}",
    {PARR("List"), PENUM("Order", "Ascending|Descending", "0")}, 0, 0, LX_FSM_ONCE, l_sort, l_sort, NULL};

static void l_reverse(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    if (lx_array_valid(a)) lx_array_reverse(a);
}
static const LxFsmActionDesc A_L_REVERSE = {"List Reverse", "List", "Turns a list back to front.", "reverse {0}",
    {PARR("List")}, 0, 0, LX_FSM_ONCE, l_reverse, l_reverse, NULL};

static void l_shuffle(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    if (lx_array_valid(a)) lx_array_shuffle(a);
}
static const LxFsmActionDesc A_L_SHUFFLE = {"List Shuffle", "List", "Puts a list in a random order: shuffling a deck, randomising spawn order.",
    "shuffle {0}",
    {PARR("List")}, 0, 0, LX_FSM_ONCE, l_shuffle, l_shuffle, NULL};

static void l_swap(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    int i, j;
    if (fsm_array_index(a, lx_fsm_int(c, 1), &i) && fsm_array_index(a, lx_fsm_int(c, 2), &j)) lx_array_swap(a, i, j);
}
static const LxFsmActionDesc A_L_SWAP = {"List Swap", "List", "Exchanges two items of a list.", "swap {0}[{1}] and {0}[{2}]",
    {PARR("List"), P("Index A", LX_FSM_INT, "0"), P("Index B", LX_FSM_INT, "1")},
    0, 0, LX_FSM_ONCE, l_swap, l_swap, NULL};

static void l_copy(LxFsmCtx *c) {
    LxArray from = fsm_list(c, 0), to = fsm_list(c, 1);
    if (lx_array_valid(from) && lx_array_valid(to)) lx_array_copy(to, from);
}
static const LxFsmActionDesc A_L_COPY = {"List Copy", "List", "Makes one list a copy of another; items are converted to the destination's type.",
    "copy {0} -> {1}",
    {PARR("From List"), PARR("To List")}, 0, 0, LX_FSM_ONCE, l_copy, l_copy, NULL};

static void l_append(LxFsmCtx *c) {
    LxArray from = fsm_list(c, 0), to = fsm_list(c, 1);
    if (!lx_array_valid(from) || !lx_array_valid(to)) return;
    int n = lx_array_length(from);
    for (int i = 0; i < n; i++) lx_array_add_string(to, lx_array_get_string(from, i));
}
static const LxFsmActionDesc A_L_APPEND = {"List Append", "List", "Adds every item of one list onto the end of another.", "append {0} onto {1}",
    {PARR("From List"), PARR("To List")}, 0, 0, LX_FSM_ONCE, l_append, l_append, NULL};

/* ==========================================================================================
 *  whole-list tests
 * ========================================================================================== */
static void l_all_any(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    int n = lx_array_valid(a) ? lx_array_length(a) : 0;
    bool want = lx_fsm_bool(c, 1);
    bool all = n > 0, any = false;
    int count = 0;
    for (int i = 0; i < n; i++) {
        bool v = lx_array_get_bool(a, i);
        if (v == want) { any = true; count++; } else { all = false; }
    }
    lx_fsm_set_bool(c, 2, all);
    lx_fsm_set_bool(c, 3, any);
    lx_fsm_set_int(c, 4, count);
    fsm_send(c, all ? 5 : (any ? 6 : 7));
}
static const LxFsmActionDesc A_L_ALL_ANY = {"List Bool Test", "List", "Checks a list of bools: are they all the value you want, are any of them, and how many?",
    "test {0} for {1}",
    {PARR("Bool List"), P("Looking For", LX_FSM_BOOL, "true"), POPT("Store All Match", LX_FSM_BOOL),
     POPT("Store Any Match", LX_FSM_BOOL), POPT("Store Match Count", LX_FSM_INT),
     PEV("All Event"), PEV("Some Event"), PEV("None Event")},
    0, 0, LX_FSM_ONCE, l_all_any, l_all_any, NULL};

static void l_contains_all_any(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0), b = fsm_list(c, 1);
    if (!lx_array_valid(a) || !lx_array_valid(b)) { fsm_send(c, 5); return; }
    int na = lx_array_length(a), nb = lx_array_length(b);
    int found = 0;
    for (int j = 0; j < nb; j++) {
        const char *want = lx_array_get_string(b, j);
        char keep[LX_REF_MAX];
        lx_strcpy(keep, sizeof keep, want);
        for (int i = 0; i < na; i++) {
            if (!strcmp(lx_array_get_string(a, i), keep)) { found++; break; }
        }
    }
    bool all = nb > 0 && found == nb, any = found > 0;
    lx_fsm_set_bool(c, 2, all);
    lx_fsm_set_bool(c, 3, any);
    lx_fsm_set_int(c, 4, found);
    fsm_send(c, all ? 5 : (any ? 6 : 7));
}
static const LxFsmActionDesc A_L_CONTAINS_SET = {"List Contains List", "List", "Does one list hold all, or any, of the values in another? Quest items, recipe checks.",
    "{0} contains {1}?",
    {PARR("List"), PARR("Values"), POPT("Store Contains All", LX_FSM_BOOL), POPT("Store Contains Any", LX_FSM_BOOL),
     POPT("Store Found Count", LX_FSM_INT), PEV("All Event"), PEV("Some Event"), PEV("None Event")},
    0, 0, LX_FSM_ONCE, l_contains_all_any, l_contains_all_any, NULL};

static void l_to_string(LxFsmCtx *c) {
    LxArray a = fsm_list(c, 0);
    const char *sep = lx_fsm_string(c, 1);
    char out[LX_REF_MAX];
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
static const LxFsmActionDesc A_L_TO_STRING = {"List To String", "List", "Writes a whole list out as one piece of text: handy for debugging and for inventories.",
    "{0} -> {2}",
    {PARR("List"), P("Separator", LX_FSM_STRING, ", "), POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, l_to_string, l_to_string, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_list(void) {
    static const LxFsmActionDesc *all[] = {
        &A_L_COUNT, &A_L_CLEAR, &A_L_RESIZE,
        &A_L_ADD, &A_L_ADD_UNIQUE, &A_L_INSERT, &A_L_REMOVE_AT, &A_L_REMOVE_VALUE, &A_L_POP,
        &A_L_GET, &A_L_SET, &A_L_FIRST_LAST, &A_L_RANDOM, &A_L_NEXT,
        &A_L_INDEX_OF, &A_L_CONTAINS,
        &A_L_SORT, &A_L_REVERSE, &A_L_SHUFFLE, &A_L_SWAP, &A_L_COPY, &A_L_APPEND,
        &A_L_ALL_ANY, &A_L_CONTAINS_SET, &A_L_TO_STRING,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
