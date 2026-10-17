#ifndef __MANAGE_DYN_TAGS_H__
#define __MANAGE_DYN_TAGS_H__ 1

#include "mango/common/types.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Dynamic tags.
 *
 * Every monitor has config.tag_num tag slots but only the ones in
 * Monitor.active_tags_mask exist. The first config.initial_tags_count are
 * always active; the others ("dynamic" tags) are opened with add_tag and
 * closed with remove_tag, or automatically once empty when
 * config.auto_remove_empty_tags is set.
 */

/* grace period, in ms, of a dynamic tag created together with a spawn */
#define DYN_TAG_SPAWN_GRACE_MS 30000

bool monitor_tag_active(const Monitor *m, uint32_t tag);
/* false when mask names tags but none of them is active on m */
bool monitor_tags_allowed(const Monitor *m, uint32_t mask);
/* number of active tags on m */
int32_t monitor_active_tag_count(const Monitor *m);
/* highest active tag (1-based) of m: the tags above it do not exist yet */
uint32_t monitor_last_active_tag(const Monitor *m);
/* lowest dynamic tag (1-based) not active on m, or -1 */
int32_t dyn_tag_first_free(const Monitor *m);
/* highest active dynamic tag (1-based) of m, or -1 */
int32_t dyn_tag_last_active(const Monitor *m);

/* open a dynamic tag on m: returns its index (1-based) or -1 when none is
 * free. name may be NULL. pending_spawn starts the grace period for a client
 * that is about to be spawned on it. */
int32_t dyn_tag_create(Monitor *m, const char *name, bool pending_spawn);

/* does a dynamic tag still hold something that keeps it alive: a client, or
 * a tag-bound scratchpad? */
bool dyn_tag_has_clients(Monitor *m, uint32_t tag_mask);

/* close the tag: the caller already checked it can be (or evicted its
 * clients). Moves the view away when needed and refreshes the status. */
void dyn_tag_deactivate(Monitor *m, int32_t tag);

/* kill the clients and tag-bound scratchpads of a tag and release the other
 * scratchpads from it. Killed client ids go to ids (up to cap), count in n. */
void dyn_tag_evict_clients(Monitor *m, uint32_t tag_mask, uint32_t *ids,
						   int32_t cap, int32_t *n);

/* drop empty dynamic tags of m (auto_remove_empty_tags) */
void cleanup_empty_dynamic_tags(Monitor *m);

/* map the dynamic tags of a client leaving a closed monitor onto free tags of
 * dest. tag_mapping is shared by all the clients of that monitor. */
uint32_t dyn_tag_remap_to_monitor(Client *c, Monitor *dest,
								  uint32_t tag_mapping[]);

#endif
