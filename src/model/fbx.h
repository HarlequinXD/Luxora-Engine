/* FBX reader: binary (6.x/7.x, 32 and 64-bit offsets) and ASCII files into a node tree. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct FbxProp {
    char type;               /* 'S' string, 'R' raw, 'I' integer, 'D' number, 'A' array */
    int64_t i;
    double d;
    const char *s;           /* S / R (not null-terminated for R) */
    uint32_t len;
    char atype;              /* arrays: 'd' double, 'f' float, 'i' int32, 'l' int64, 'b' bool */
    const void *arr;
    uint32_t count;
} FbxProp;

typedef struct FbxNode {
    const char *name;
    FbxProp *props;
    int nprops;
    struct FbxNode **kids;
    int nkids;
} FbxNode;

typedef struct FbxDoc FbxDoc;
FbxDoc *fbx_parse(const uint8_t *data, size_t size, char *err, size_t err_cap);
void fbx_free(FbxDoc *d);
FbxNode *fbx_root(FbxDoc *d);
int fbx_version(const FbxDoc *d);

FbxNode *fbx_child(const FbxNode *n, const char *name);
double fbx_num(const FbxProp *p);               /* I or D */
const char *fbx_str(FbxDoc *d, const FbxProp *p); /* null-terminated copy (arena) */
double fbx_arr_d(const FbxProp *p, uint32_t i);
int64_t fbx_arr_i(const FbxProp *p, uint32_t i);
