#ifndef SPRITE_CACHE_H
#define SPRITE_CACHE_H

#include <stdint.h>

typedef struct SpriteCacheEntry_s SpriteCacheEntry;

// A sprite cache.
typedef struct {
	int sprite_size;                // size of a sprite in bytes
	int max_sprites;                // maximum number of sprites in cache
	int num_buckets;                // number of hash table buckets (should be pow2)
	int32_t cur_tick;               // current tick (frame counter)
	int32_t tick_cutoff;            // tick which marks sprites old enough to remove with sprite_cache_pop
	uint8_t *sprites;               // pixel memory (for all sprites)
	uint16_t *free_sprite_indices;
	int num_sprites;				// number of sprites currently in cache
	SpriteCacheEntry *buckets;      // hashtable of the sprite entries
	int sprite_shift;               // log2(sprite_size): slot = offset >> shift
	uint8_t *slot_tick;             // LRU tick per pixel slot (max_sprites):
	                                // per SLOT, not per bucket, so a caller
	                                // resolving through a direct table (no
	                                // bucket probe) can still mark a hit
	uint16_t *dt;                   // optional direct table (key -> slot+3),
	                                // see roms.c crom_resolve: eviction
	                                // demotes an entry >= 3 back to 2
} SpriteCache;

// Pixel slot index of a pointer returned by lookup/insert.
static inline int sprite_cache_slot(const SpriteCache *c, const uint8_t *sprite) {
	return (int)(sprite - c->sprites) >> c->sprite_shift;
}

void sprite_cache_init(SpriteCache *c, int sprite_size, int max_sprites);
void sprite_cache_reset(SpriteCache *c);
void sprite_cache_tick(SpriteCache *c);
uint8_t* sprite_cache_lookup(SpriteCache *c, uint32_t key);
uint8_t* sprite_cache_insert(SpriteCache *c, uint32_t key);
void sprite_cache_pop(SpriteCache *c);

#endif /* SPRITE_CACHE_H */
