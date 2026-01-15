//
//  S3FIFO.c (Adaptive Version)
//  libCacheSim
//
//  Implements O(1) periodic resizing based on Ghost Queue pressure.
//

#include "dataStructure/hashtable/hashtable.h"
#include "libCacheSim/evictionAlgo.h"
#include <math.h> // For fabs

#ifdef __cplusplus
extern "C" {
#endif

// --- TUNING KNOBS ---
#define EPOCH_SIZE 1000000        // How often to re-evaluate (reqs)
#define BASE_SMALL_RATIO 0.3933  // From your distilled equation
#define MIN_SMALL_RATIO 0.01
#define MAX_SMALL_RATIO 0.90

typedef struct {
  int64_t interval_reqs;
  int64_t hits_small;
  int64_t hits_main;
  int64_t hits_ghost;
} epoch_stats_t;

typedef struct {
  cache_t *small_fifo;
  cache_t *ghost_fifo;
  cache_t *main_fifo;
  bool hit_on_ghost;

  int move_to_main_threshold;
  double small_size_ratio;
  double ghost_size_ratio;

  bool has_evicted;
  request_t *req_local;

  // [NEW] Statistics for adaptivity
  epoch_stats_t stats;
} S3FIFO_params_t;

static const char *DEFAULT_CACHE_PARAMS =
    "small-size-ratio=0.10,ghost-size-ratio=0.90,move-to-main-threshold=2";

// ***********************************************************************
// **** function declarations                       ****
// ***********************************************************************
static void S3FIFO_free(cache_t *cache);
static bool S3FIFO_get(cache_t *cache, const request_t *req);
static cache_obj_t *S3FIFO_find(cache_t *cache, const request_t *req,
                                const bool update_cache);
static cache_obj_t *S3FIFO_insert(cache_t *cache, const request_t *req);
static cache_obj_t *S3FIFO_to_evict(cache_t *cache, const request_t *req);
static void S3FIFO_evict(cache_t *cache, const request_t *req);
static bool S3FIFO_remove(cache_t *cache, const obj_id_t obj_id);
static inline int64_t S3FIFO_get_occupied_byte(const cache_t *cache);
static inline int64_t S3FIFO_get_n_obj(const cache_t *cache);
static inline bool S3FIFO_can_insert(cache_t *cache, const request_t *req);
static void S3FIFO_parse_params(cache_t *cache,
                                const char *cache_specific_params);
static void S3FIFO_evict_small(cache_t *cache, const request_t *req);
static void S3FIFO_evict_main(cache_t *cache, const request_t *req);

// [NEW] Adaptivity Helper Functions
static void S3FIFO_reconfigure(cache_t *cache);
static void S3FIFO_resize(cache_t *cache, double new_small_ratio);

// ***********************************************************************
// **** end user facing functions                   ****
// ***********************************************************************

cache_t *S3FIFO_init(const common_cache_params_t ccache_params,
                     const char *cache_specific_params) {
  cache_t *cache =
      cache_struct_init("S3FIFO", ccache_params, cache_specific_params);
  cache->cache_init = S3FIFO_init;
  cache->cache_free = S3FIFO_free;
  cache->get = S3FIFO_get;
  cache->find = S3FIFO_find;
  cache->insert = S3FIFO_insert;
  cache->evict = S3FIFO_evict;
  cache->remove = S3FIFO_remove;
  cache->to_evict = S3FIFO_to_evict;
  cache->get_n_obj = S3FIFO_get_n_obj;
  cache->get_occupied_byte = S3FIFO_get_occupied_byte;
  cache->can_insert = S3FIFO_can_insert;

  cache->obj_md_size = 0;

  cache->eviction_params = malloc(sizeof(S3FIFO_params_t));
  memset(cache->eviction_params, 0, sizeof(S3FIFO_params_t));
  S3FIFO_params_t *params = (S3FIFO_params_t *)cache->eviction_params;
  params->req_local = new_request();
  params->hit_on_ghost = false;

  S3FIFO_parse_params(cache, DEFAULT_CACHE_PARAMS);
  if (cache_specific_params != NULL) {
    S3FIFO_parse_params(cache, cache_specific_params);
  }

  int64_t small_fifo_size =
      (int64_t)ccache_params.cache_size * params->small_size_ratio;
  int64_t main_fifo_size = ccache_params.cache_size - small_fifo_size;
  int64_t ghost_fifo_size =
      (int64_t)(ccache_params.cache_size * params->ghost_size_ratio);

  common_cache_params_t ccache_params_local = ccache_params;
  ccache_params_local.cache_size = small_fifo_size;
  params->small_fifo = FIFO_init(ccache_params_local, NULL);
  params->has_evicted = false;

  if (ghost_fifo_size > 0) {
    ccache_params_local.cache_size = ghost_fifo_size;
    params->ghost_fifo = FIFO_init(ccache_params_local, NULL);
    snprintf(params->ghost_fifo->cache_name, CACHE_NAME_ARRAY_LEN,
             "FIFO-ghost");
  } else {
    params->ghost_fifo = NULL;
  }

  ccache_params_local.cache_size = main_fifo_size;
  params->main_fifo = FIFO_init(ccache_params_local, NULL);

  snprintf(cache->cache_name, CACHE_NAME_ARRAY_LEN, "S3FIFO-%.4lf-%d",
           params->small_size_ratio, params->move_to_main_threshold);

  return cache;
}

static void S3FIFO_free(cache_t *cache) {
  S3FIFO_params_t *params = (S3FIFO_params_t *)cache->eviction_params;
  free_request(params->req_local);
  params->small_fifo->cache_free(params->small_fifo);
  if (params->ghost_fifo != NULL) {
    params->ghost_fifo->cache_free(params->ghost_fifo);
  }
  params->main_fifo->cache_free(params->main_fifo);
  free(cache->eviction_params);
  cache_struct_free(cache);
}

// [MODIFIED] Check trigger and update stats
static bool S3FIFO_get(cache_t *cache, const request_t *req) {
  S3FIFO_params_t *params = (S3FIFO_params_t *)cache->eviction_params;

  // 1. Adaptivity Trigger
  params->stats.interval_reqs++;
  if (params->stats.interval_reqs >= EPOCH_SIZE) {
      S3FIFO_reconfigure(cache);
  }

  DEBUG_ASSERT(params->small_fifo->get_occupied_byte(params->small_fifo) +
                   params->main_fifo->get_occupied_byte(params->main_fifo) <=
               cache->cache_size);

  bool cache_hit = cache_get_base(cache, req);

  return cache_hit;
}

// [MODIFIED] Count hits
static cache_obj_t *S3FIFO_find(cache_t *cache, const request_t *req,
                                const bool update_cache) {
  S3FIFO_params_t *params = (S3FIFO_params_t *)cache->eviction_params;

  if (!update_cache) {
    cache_obj_t *obj = params->small_fifo->find(params->small_fifo, req, false);
    if (obj != NULL) return obj;
    obj = params->main_fifo->find(params->main_fifo, req, false);
    if (obj != NULL) return obj;
    return NULL;
  }

  params->hit_on_ghost = false;

  // Check Small
  cache_obj_t *obj = params->small_fifo->find(params->small_fifo, req, true);
  if (obj != NULL) {
    obj->S3FIFO.freq += 1;
    params->stats.hits_small++; // [NEW] Track Hit
    return obj;
  }

  // Check Ghost
  if (params->ghost_fifo != NULL &&
      params->ghost_fifo->remove(params->ghost_fifo, req->obj_id)) {
    params->hit_on_ghost = true;
    params->stats.hits_ghost++; // [NEW] Track Hit
  }

  // Check Main
  obj = params->main_fifo->find(params->main_fifo, req, true);
  if (obj != NULL) {
    obj->S3FIFO.freq += 1;
    params->stats.hits_main++; // [NEW] Track Hit
  }

  return obj;
}

// ... Insert, to_evict, etc. remain UNCHANGED ...
static cache_obj_t *S3FIFO_insert(cache_t *cache, const request_t *req) {
  S3FIFO_params_t *params = (S3FIFO_params_t *)cache->eviction_params;
  cache_obj_t *obj = NULL;
  cache_t *small_fifo = params->small_fifo;
  cache_t *main_fifo = params->main_fifo;

  if (params->hit_on_ghost) {
    params->hit_on_ghost = false;
    obj = main_fifo->insert(main_fifo, req);
  } else {
    if (req->obj_size >= small_fifo->cache_size) {
      return NULL;
    }
    if (!params->has_evicted &&
        small_fifo->get_occupied_byte(small_fifo) >= small_fifo->cache_size) {
      obj = main_fifo->insert(main_fifo, req);
    } else {
      obj = small_fifo->insert(small_fifo, req);
    }
  }
  obj->S3FIFO.freq = 0;
  return obj;
}

static cache_obj_t *S3FIFO_to_evict(cache_t *cache, const request_t *req) {
  assert(false);
  return NULL;
}

// ... Eviction logic remains standard, it will respect new sizes automatically ...
static void S3FIFO_evict_small(cache_t *cache, const request_t *req) {
  S3FIFO_params_t *params = (S3FIFO_params_t *)cache->eviction_params;
  cache_t *small_fifo = params->small_fifo;
  cache_t *ghost_fifo = params->ghost_fifo;
  cache_t *main_fifo = params->main_fifo;
  bool has_evicted = false;
  while (!has_evicted && small_fifo->get_occupied_byte(small_fifo) > 0) {
    cache_obj_t *obj_to_evict = small_fifo->to_evict(small_fifo, req);
    DEBUG_ASSERT(obj_to_evict != NULL);
    copy_cache_obj_to_request(params->req_local, obj_to_evict);
    if (obj_to_evict->S3FIFO.freq >= params->move_to_main_threshold) {
      main_fifo->insert(main_fifo, params->req_local);
    } else {
      if (ghost_fifo != NULL) ghost_fifo->get(ghost_fifo, params->req_local);
      has_evicted = true;
    }
    bool removed = small_fifo->remove(small_fifo, params->req_local->obj_id);
    DEBUG_ASSERT(removed);
  }
}

static void S3FIFO_evict_main(cache_t *cache, const request_t *req) {
  S3FIFO_params_t *params = (S3FIFO_params_t *)cache->eviction_params;
  cache_t *main_fifo = params->main_fifo;
  bool has_evicted = false;
  while (!has_evicted && main_fifo->get_occupied_byte(main_fifo) > 0) {
    cache_obj_t *obj_to_evict = main_fifo->to_evict(main_fifo, req);
    DEBUG_ASSERT(obj_to_evict != NULL);
    int freq = obj_to_evict->S3FIFO.freq;
    copy_cache_obj_to_request(params->req_local, obj_to_evict);
    if (freq >= 1) {
      main_fifo->remove(main_fifo, obj_to_evict->obj_id);
      obj_to_evict = NULL;
      cache_obj_t *new_obj = main_fifo->insert(main_fifo, params->req_local);
      new_obj->S3FIFO.freq = MIN(freq, 3) - 1;
    } else {
      bool removed = main_fifo->remove(main_fifo, obj_to_evict->obj_id);
      DEBUG_ASSERT(removed);
      has_evicted = true;
    }
  }
}

static void S3FIFO_evict(cache_t *cache, const request_t *req) {
  S3FIFO_params_t *params = (S3FIFO_params_t *)cache->eviction_params;
  params->has_evicted = true;
  cache_t *small_fifo = params->small_fifo;
  cache_t *main_fifo = params->main_fifo;

  // NOTE: This logic naturally handles resizing!
  // If we shrank Small Queue, get_occupied > cache_size, so we evict from Small.
  if (main_fifo->get_occupied_byte(main_fifo) > main_fifo->cache_size ||
      small_fifo->get_occupied_byte(small_fifo) == 0) {
    S3FIFO_evict_main(cache, req);
  } else {
    S3FIFO_evict_small(cache, req);
  }
}

static bool S3FIFO_remove(cache_t *cache, const obj_id_t obj_id) {
  S3FIFO_params_t *params = (S3FIFO_params_t *)cache->eviction_params;
  bool removed = false;
  removed = removed || params->small_fifo->remove(params->small_fifo, obj_id);
  removed = removed || (params->ghost_fifo &&
                        params->ghost_fifo->remove(params->ghost_fifo, obj_id));
  removed = removed || params->main_fifo->remove(params->main_fifo, obj_id);
  return removed;
}

static inline int64_t S3FIFO_get_occupied_byte(const cache_t *cache) {
  S3FIFO_params_t *params = (S3FIFO_params_t *)cache->eviction_params;
  return params->small_fifo->get_occupied_byte(params->small_fifo) +
         params->main_fifo->get_occupied_byte(params->main_fifo);
}

static inline int64_t S3FIFO_get_n_obj(const cache_t *cache) {
  S3FIFO_params_t *params = (S3FIFO_params_t *)cache->eviction_params;
  return params->small_fifo->get_n_obj(params->small_fifo) +
         params->main_fifo->get_n_obj(params->main_fifo);
}

static inline bool S3FIFO_can_insert(cache_t *cache, const request_t *req) {
  S3FIFO_params_t *params = (S3FIFO_params_t *)cache->eviction_params;
  return req->obj_size <= params->small_fifo->cache_size &&
         cache_can_insert_default(cache, req);
}

// ***********************************************************************
// **** ADAPTIVITY IMPLEMENTATION                     ****
// ***********************************************************************

// [NEW] The "Brain"
static void S3FIFO_reconfigure(cache_t *cache) {
  S3FIFO_params_t *params = (S3FIFO_params_t *)cache->eviction_params;

  // 1. Calculate Inputs
  double total_activity = (double)params->stats.hits_small +
                          (double)params->stats.hits_main +
                          (double)params->stats.hits_ghost + 1.0; // Avoid 0 div

  double ghost_pressure = (double)params->stats.hits_ghost / total_activity;

  // 2. The Distilled Equation: S = Base + Pressure
  double target_s = BASE_SMALL_RATIO + ghost_pressure;

  // 3. Clamp
  if (target_s < MIN_SMALL_RATIO) target_s = MIN_SMALL_RATIO;
  if (target_s > MAX_SMALL_RATIO) target_s = MAX_SMALL_RATIO;

  // 4. Actuate
  if (fabs(target_s - params->small_size_ratio) > 0.01) {
    S3FIFO_resize(cache, target_s);
    // Optional: Print readjustment for debugging
    // printf("[Adaptive] GhostP: %.2f -> New S: %.2f\n", ghost_pressure, target_s);
  }

  // 5. Reset Stats
  memset(&params->stats, 0, sizeof(epoch_stats_t));
}

// [NEW] The "Muscle"
static void S3FIFO_resize(cache_t *cache, double new_small_ratio) {
  S3FIFO_params_t *params = (S3FIFO_params_t *)cache->eviction_params;

  params->small_size_ratio = new_small_ratio;

  int64_t total_size = cache->cache_size;
  int64_t new_small_size = (int64_t)(total_size * params->small_size_ratio);
  int64_t new_main_size = total_size - new_small_size;

  // Simply update the limits.
  // The eviction logic will handle the actual object movement lazily.
  params->small_fifo->cache_size = new_small_size;
  params->main_fifo->cache_size = new_main_size;
}

// ... Parse Params remaining UNCHANGED ...
static const char *S3FIFO_current_params(S3FIFO_params_t *params) {
  static __thread char params_str[128];
  snprintf(params_str, 128,
           "small-size-ratio=%.4lf,ghost-size-ratio=%.4lf,move-to-main-"
           "threshold=%d\n",
           params->small_size_ratio, params->ghost_size_ratio,
           params->move_to_main_threshold);
  return params_str;
}

static void S3FIFO_parse_params(cache_t *cache,
                                const char *cache_specific_params) {
  S3FIFO_params_t *params = (S3FIFO_params_t *)(cache->eviction_params);
  char *params_str = strdup(cache_specific_params);
  char *old_params_str = params_str;
  while (params_str != NULL && params_str[0] != '\0') {
    char *key = strsep((char **)&params_str, "=");
    char *value = strsep((char **)&params_str, ",");
    while (params_str != NULL && *params_str == ' ') {
      params_str++;
    }
    if (strcasecmp(key, "fifo-size-ratio") == 0 ||
        strcasecmp(key, "small-size-ratio") == 0) {
      params->small_size_ratio = strtod(value, NULL);
    } else if (strcasecmp(key, "ghost-size-ratio") == 0) {
      params->ghost_size_ratio = strtod(value, NULL);
    } else if (strcasecmp(key, "move-to-main-threshold") == 0) {
      params->move_to_main_threshold = atoi(value);
    } else if (strcasecmp(key, "print") == 0) {
      printf("parameters: %s\n", S3FIFO_current_params(params));
      exit(0);
    } else {
      ERROR("%s does not have parameter %s\n", cache->cache_name, key);
      exit(1);
    }
  }
  free(old_params_str);
}

#ifdef __cplusplus
}
#endif