/* Imported models: shared between the FBX loader (model.c), avatars (avatar.c) and animation (anim.c). */
#pragma once
#include "../lx_internal.h"

typedef struct ModelNode {
    char name[64];
    int parent;                /* -1 = top level (under the object the model is placed on) */
    LxVec3 pos;                /* rest pose, local */
    LxQuat rot;
    LxVec3 scale;
    bool bone;
    int part_first, part_count;
} ModelNode;

typedef struct ModelPart {
    int node;
    LxMesh *mesh;              /* in the node's local space (bind pose for skinned parts) */
    char material[LX_REF_MAX];
    char name[64];
} ModelPart;

typedef struct ModelClip {
    char name[64];
    float fps, length;
    int frames;
    int tracks;
    int *node;                 /* track -> node index of this model */
    LxVec3 *pos;               /* [frame * tracks + track] */
    LxQuat *rot;
    LxVec3 *scl;
} ModelClip;

struct LxModel {
    char ref[LX_REF_MAX];
    LxModelImport imp;
    ModelNode *nodes;
    int node_count;
    ModelPart *parts;
    int part_count, part_cap;
    ModelClip *clips;
    int clip_count, clip_cap;
    int *bones;                /* node indices of the skeleton */
    int bone_count, bone_cap;
    int avatar[LX_HB_COUNT];   /* node index per human bone, -1 = unmapped */
    LxAabb bounds;
    /* rest pose in model space */
    LxVec3 *rest_wpos;
    LxQuat *rest_wrot;
    /* humanoid retargeting: T-pose (normalised) rotations of mapped bones in a canonical frame */
    bool humanoid_ok;
    LxQuat tpose[LX_HB_COUNT];   /* canonical-space world rotation of each human bone in T-pose */
    LxQuat to_canon;             /* model space -> canonical (character faces +Z, up +Y, left +X) */
    float hips_height;           /* hips height above the feet in the rest pose */
    LxVec3 hips_rest;            /* hips rest position (model space) */
};

LxModel *lx_model_load_file(const char *path, const char *ref, char *err, size_t err_cap);
void lx_model_free(LxModel *m);
void lx_model_prepare_avatar(LxModel *m);   /* resolves imp.avatar names, computes the T-pose data */
uint32_t lx_model_generation(void);
int lx_model_find_node(const LxModel *m, const char *name);
LxMesh *lx_model_part_mesh(LxModel *m, int part);
