/* Crowd simulation: structure-of-arrays agents, spatial hash grid rebuilt per step with a
 * counting sort, steering (seek + arrival + separation + obstacle avoidance) run in parallel. */
#include "../lx_internal.h"

typedef struct Obstacle { LxVec3 c; float r; } Obstacle;

struct LxCrowd {
    int count, cap;
    LxVec3 *pos, *vel, *target, *new_vel;
    float *radius, *max_speed;
    /* spatial hash */
    float cell;
    int bucket_count;           /* power of two */
    int *bucket_start;          /* bucket_count + 1 */
    int *sorted;                /* agent indices sorted by bucket */
    uint32_t *agent_bucket;
    Obstacle *obstacles;
    int obstacle_count, obstacle_cap;
    float dt;
};

LxCrowd *lx_crowd_create(int max_agents) {
    LxCrowd *c = lx_calloc(1, sizeof *c);
    c->cap = max_agents > 0 ? max_agents : 1;
    c->pos = lx_alloc(sizeof(LxVec3) * (size_t)c->cap);
    c->vel = lx_calloc((size_t)c->cap, sizeof(LxVec3));
    c->new_vel = lx_calloc((size_t)c->cap, sizeof(LxVec3));
    c->target = lx_alloc(sizeof(LxVec3) * (size_t)c->cap);
    c->radius = lx_alloc(sizeof(float) * (size_t)c->cap);
    c->max_speed = lx_alloc(sizeof(float) * (size_t)c->cap);
    c->bucket_count = 1;
    while (c->bucket_count < c->cap * 2) c->bucket_count <<= 1;
    c->bucket_start = lx_alloc(sizeof(int) * (size_t)(c->bucket_count + 1));
    c->sorted = lx_alloc(sizeof(int) * (size_t)c->cap);
    c->agent_bucket = lx_alloc(sizeof(uint32_t) * (size_t)c->cap);
    c->cell = 1.0f;
    return c;
}

void lx_crowd_destroy(LxCrowd *c) {
    if (!c) return;
    lx_free(c->pos); lx_free(c->vel); lx_free(c->new_vel); lx_free(c->target);
    lx_free(c->radius); lx_free(c->max_speed);
    lx_free(c->bucket_start); lx_free(c->sorted); lx_free(c->agent_bucket);
    lx_free(c->obstacles);
    lx_free(c);
}

int lx_crowd_add(LxCrowd *c, LxVec3 pos, float radius, float max_speed) {
    if (c->count >= c->cap) return -1;
    int i = c->count++;
    c->pos[i] = pos;
    c->vel[i] = lx_v3(0, 0, 0);
    c->target[i] = pos;
    c->radius[i] = radius;
    c->max_speed[i] = max_speed;
    float cell = radius * 4.0f;
    if (cell > c->cell) c->cell = cell;
    return i;
}

void lx_crowd_set_target(LxCrowd *c, int a, LxVec3 t) { if (a >= 0 && a < c->count) c->target[a] = t; }
int lx_crowd_count(const LxCrowd *c) { return c->count; }
LxVec3 lx_crowd_position(const LxCrowd *c, int a) { return c->pos[a]; }
LxVec3 lx_crowd_velocity(const LxCrowd *c, int a) { return c->vel[a]; }
int lx_crowd_positions(const LxCrowd *c, const LxVec3 **pos, const LxVec3 **vel) {
    *pos = c->pos;
    *vel = c->vel;
    return c->count;
}

void lx_crowd_add_obstacle(LxCrowd *c, LxVec3 center, float radius) {
    Obstacle o = {center, radius};
    LX_VEC_PUSH(c->obstacles, c->obstacle_count, c->obstacle_cap, o);
}

static inline uint32_t cell_hash(int x, int z, int mask) {
    uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)z * 19349663u;
    return h & (uint32_t)mask;
}

static void steer_job(void *user, int begin, int end, int thread) {
    LX_UNUSED(thread);
    LxCrowd *c = user;
    int mask = c->bucket_count - 1;
    float inv_cell = 1.0f / c->cell;
    for (int i = begin; i < end; i++) {
        LxVec3 p = c->pos[i];
        float maxs = c->max_speed[i];
        /* seek with arrival */
        LxVec3 to = lx_sub3(c->target[i], p);
        to.y = 0;
        float dist = lx_len3(to);
        LxVec3 desired = lx_v3(0, 0, 0);
        if (dist > 0.05f) {
            float speed = dist < 2.0f ? maxs * (dist / 2.0f) : maxs;
            desired = lx_scale3(to, speed / dist);
        }
        /* separation from neighbours in the 3x3 cell block */
        LxVec3 sep = lx_v3(0, 0, 0);
        int cx = (int)floorf(p.x * inv_cell), cz = (int)floorf(p.z * inv_cell);
        float ri = c->radius[i];
        int checked = 0;
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) {
                uint32_t b = cell_hash(cx + dx, cz + dz, mask);
                for (int k = c->bucket_start[b]; k < c->bucket_start[b + 1] && checked < 48; k++) {
                    int j = c->sorted[k];
                    if (j == i) continue;
                    LxVec3 d = lx_sub3(p, c->pos[j]);
                    d.y = 0;
                    float min_d = (ri + c->radius[j]) * 1.6f;
                    float d2 = lx_len3sq(d);
                    if (d2 < min_d * min_d && d2 > 1e-8f) {
                        float dl = sqrtf(d2);
                        sep = lx_madd3(sep, d, (min_d - dl) / (min_d * dl));
                        checked++;
                    }
                }
            }
        /* obstacles */
        LxVec3 avoid = lx_v3(0, 0, 0);
        for (int o = 0; o < c->obstacle_count; o++) {
            LxVec3 d = lx_sub3(p, c->obstacles[o].c);
            d.y = 0;
            float r = c->obstacles[o].r + ri + 0.5f;
            float dl = lx_len3(d);
            if (dl < r && dl > 1e-5f) avoid = lx_madd3(avoid, d, (r - dl) / (r * dl) * 3.0f);
        }
        LxVec3 v = lx_add3(desired, lx_scale3(lx_add3(sep, avoid), maxs * 1.5f));
        float vl = lx_len3(v);
        if (vl > maxs) v = lx_scale3(v, maxs / vl);
        /* smooth acceleration */
        c->new_vel[i] = lx_lerp3(c->vel[i], v, lx_minf(1.0f, c->dt * 8.0f));
    }
}

static void integrate_job(void *user, int begin, int end, int thread) {
    LX_UNUSED(thread);
    LxCrowd *c = user;
    for (int i = begin; i < end; i++) {
        c->vel[i] = c->new_vel[i];
        c->pos[i] = lx_madd3(c->pos[i], c->vel[i], c->dt);
    }
}

void lx_crowd_update(LxCrowd *c, float dt) {
    if (c->count == 0) return;
    c->dt = lx_clampf(dt, 0.0f, 0.1f);
    int mask = c->bucket_count - 1;
    float inv_cell = 1.0f / c->cell;
    /* counting sort agents into hash buckets */
    memset(c->bucket_start, 0, sizeof(int) * (size_t)(c->bucket_count + 1));
    for (int i = 0; i < c->count; i++) {
        uint32_t b = cell_hash((int)floorf(c->pos[i].x * inv_cell), (int)floorf(c->pos[i].z * inv_cell), mask);
        c->agent_bucket[i] = b;
        c->bucket_start[b + 1]++;
    }
    for (int b = 0; b < c->bucket_count; b++) c->bucket_start[b + 1] += c->bucket_start[b];
    int *fill = lx_alloc(sizeof(int) * (size_t)c->bucket_count);
    memcpy(fill, c->bucket_start, sizeof(int) * (size_t)c->bucket_count);
    for (int i = 0; i < c->count; i++) c->sorted[fill[c->agent_bucket[i]]++] = i;
    lx_free(fill);
    lx_parallel_for(c->count, 1024, steer_job, c);
    lx_parallel_for(c->count, 4096, integrate_job, c);
}
