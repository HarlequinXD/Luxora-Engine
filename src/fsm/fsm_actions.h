/* Shared helpers for the built-in State Graph action library.
 *
 * The actions are grouped into categories and named for what they do, so the browser reads as a list
 * of plain tasks. Each one is an LxFsmActionDesc — name, category, description, summary line and
 * parameters — plus one to three small C functions.
 *
 * Parameters are read with lx_fsm_float/int/bool/string/vec3/color/object/asset/event and results are
 * stored with lx_fsm_set_*. A parameter marked LX_FSM_P_OUT must be bound to a variable; one marked
 * LX_FSM_P_OPTIONAL may be left as None (test it with lx_fsm_is_none).
 */
#ifndef LX_FSM_ACTIONS_H
#define LX_FSM_ACTIONS_H

#include "../lx_internal.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <float.h>

/* ---- parameter shorthands ---------------------------------------------------------------- */
#define P(n, t, d)            {n, t, d, 0, NULL, NULL}
#define PT(n, t, d, tip)      {n, t, d, 0, NULL, tip}
#define POUT(n, t)            {n, t, "", LX_FSM_P_OUT, NULL, NULL}
#define POUTT(n, t, tip)      {n, t, "", LX_FSM_P_OUT, NULL, tip}
#define POPT(n, t)            {n, t, "", LX_FSM_P_OUT | LX_FSM_P_OPTIONAL, NULL, NULL}
#define POPTT(n, t, tip)      {n, t, "", LX_FSM_P_OUT | LX_FSM_P_OPTIONAL, NULL, tip}
#define PIN_OPT(n, t)         {n, t, "", LX_FSM_P_OPTIONAL, NULL, NULL}
#define PEV(n)                {n, LX_FSM_EVENT, "", LX_FSM_P_OPTIONAL, NULL, NULL}
#define PENUM(n, opts, d)     {n, LX_FSM_ENUM, d, 0, opts, NULL}
#define PENUMT(n, opts, d, t) {n, LX_FSM_ENUM, d, 0, opts, t}
#define PASSET(n, kind)       {n, LX_FSM_ASSET, "", 0, kind, NULL}
#define PASSETT(n, kind, tip) {n, LX_FSM_ASSET, "", 0, kind, tip}
#define POBJ(n)               {n, LX_FSM_OBJECT, "Owner", 0, NULL, NULL}
#define POBJT(n, tip)         {n, LX_FSM_OBJECT, "Owner", 0, NULL, tip}
#define POBJN(n)              {n, LX_FSM_OBJECT, "None", LX_FSM_P_OPTIONAL, NULL, NULL}
#define POBJNT(n, tip)        {n, LX_FSM_OBJECT, "None", LX_FSM_P_OPTIONAL, NULL, tip}
#define PARR(n)               {n, LX_FSM_ARRAY, "", 0, NULL, NULL}
#define PARRT(n, tip)         {n, LX_FSM_ARRAY, "", 0, NULL, tip}

/* enum option strings used by more than one action */
#define FSM_SPACE  "Self|World"
#define FSM_EASES  "Linear|Ease In|Ease Out|Ease In Out|Back Out|Elastic Out|Bounce Out"
#define FSM_AXIS   "X|Y|Z"
#define FSM_AXIS4  "X|Y|Z|W"

/* ---- small shared helpers ---------------------------------------------------------------- */
int fsm_stricmp(const char *a, const char *b);
void fsm_send(LxFsmCtx *c, int p);                 /* send the event in parameter p (no-op when None) */
LxEntity fsm_obj(LxFsmCtx *c, int p);              /* object parameter, 0 when None */
LxEase fsm_ease(int index);
/* clamp an array index; returns false when the array or index is unusable */
bool fsm_array_index(LxArray a, int i, int *out);
/* Resolve an index that a graph supplied against a length, counting a negative one back from the end.
 * Clamped in 64 bits first, so INT_MIN / INT_MAX cannot overflow into a valid-looking offset. */
LX_INLINE int fsm_index(int i, int length) {
    long long v = (long long)i;
    if (v < 0) v += length;
    if (v < 0) return -1;
    if (v > length) return length;
    return (int)v;
}
/* the usable run starting at `start`, for a caller-supplied count (0 or less = to the end) */
LX_INLINE int fsm_count_from(int start, int count, int length) {
    long long room = (long long)length - start;
    if (room < 0) room = 0;
    if (count <= 0 || (long long)count > room) return (int)room;
    return count;
}

#endif
