#include "mango/manage/dyn_tags.h"
#include "mango/common/server.h"
#include "mango/common/util.h"
#include "mango/config/parse.h"
#include "mango/dispatch/bind.h"
#include "mango/ext-protocol/ext-workspace.h"
#include "mango/ipc/ipc.h"
#include "mango/layout/arrange.h"
#include "mango/manage/client.h"
#include "mango/manage/monitor.h"
#include <stdio.h>
#include <string.h>

bool monitor_tag_active(const Monitor *m, uint32_t tag) {
	return m && tag >= 1 && tag <= (uint32_t)config.tag_num &&
		   (m->active_tags_mask & (1u << (tag - 1)));
}

bool monitor_tags_allowed(const Monitor *m, uint32_t mask) {
	if (!m || !mask)
		return true;
	/* the special tag and "every tag" are not tied to a tag slot */
	if ((mask & TAG0_MASK) || mask == (~0u & TAGMASK))
		return true;
	return (mask & m->active_tags_mask) != 0;
}

int32_t monitor_active_tag_count(const Monitor *m) {
	return m ? __builtin_popcount(m->active_tags_mask & TAGMASK) : 0;
}

uint32_t monitor_last_active_tag(const Monitor *m) {
	uint32_t active = m ? m->active_tags_mask & TAGMASK : 0;

	return active ? 32 - __builtin_clz(active) : (uint32_t)config.tag_num;
}

int32_t dyn_tag_first_free(const Monitor *m) {
	int32_t i;

	for (i = config.initial_tags_count; i < config.tag_num; i++) {
		if (!(m->active_tags_mask & (1u << i)))
			return i + 1;
	}
	return -1;
}

int32_t dyn_tag_last_active(const Monitor *m) {
	int32_t i;

	for (i = config.tag_num - 1; i >= config.initial_tags_count; i--) {
		if (m->active_tags_mask & (1u << i))
			return i + 1;
	}
	return -1;
}

/* first active tag of m (as a mask), other than the ones in skip */
static uint32_t first_active_tag_mask(const Monitor *m, uint32_t skip) {
	int32_t i;

	for (i = 0; i < config.tag_num; i++) {
		uint32_t bit = 1u << i;
		if ((m->active_tags_mask & bit) && !(skip & bit))
			return bit;
	}
	return 0;
}

/* a tag slot that was used before starts again from its configured state; the
 * layout trees stay, they belong to the slot and are freed with it */
static void dyn_tag_reset_slot(Monitor *m, uint32_t tag) {
	int32_t i;

	tag_slot_set_defaults(m, tag);
	m->pertag->no_hide[tag] = 0;
	m->pertag->no_render_border[tag] = 0;
	m->pertag->open_as_floating[tag] = 0;

	for (i = 0; i < config.tag_rules_count; i++) {
		const ConfigTagRule *tr = &config.tag_rules[i];

		if (tag_rule_matches_monitor(tr, m) &&
			(tr->id_wildcard || tr->id == (int32_t)tag))
			tag_rule_apply_to_slot(m, tr, tag);
	}
	m->pertag->ltidxs[tag] = m->pertag->config_ltidxs[tag];
}

int32_t dyn_tag_create(Monitor *m, const char *name, bool pending_spawn) {
	int32_t idx;
	uint32_t bit;

	if (!m || !m->pertag)
		return -1;

	idx = dyn_tag_first_free(m);
	if (idx < 0)
		return -1;

	bit = 1u << (idx - 1);
	m->active_tags_mask |= bit;
	m->tags_had_clients_mask &= ~bit;
	m->tags_pending_spawn_mask &= ~bit;

	dyn_tag_reset_slot(m, idx);

	m->pertag->tag_names[idx][0] = '\0';
	if (name && name[0] != '\0')
		snprintf(m->pertag->tag_names[idx], sizeof(m->pertag->tag_names[idx]),
				 "%s", name);

	if (pending_spawn) {
		m->tags_pending_spawn_mask |= bit;
		m->tag_spawn_time[idx - 1] = get_now_in_ms();
	}

	sync_workspaces_to_tag_num(m);
	return idx;
}

bool dyn_tag_has_clients(Monitor *m, uint32_t tag_mask) {
	Client *c = NULL;

	wl_list_for_each(c, &server.clients, link) {
		if (c->mon != m)
			continue;

		if (c->is_in_scratchpad && c->isminimized) {
			/* a hidden scratchpad remembers its tag in oldtags */
			if (c->is_tag_bound && (c->oldtags & tag_mask))
				return true;
			continue;
		}

		if ((c->tags & tag_mask) && (!c->is_in_scratchpad || c->is_tag_bound))
			return true;
	}
	return false;
}

/* a scratchpad that is not bound to the tag is released from it and stays
 * available on the others */
static void scratchpad_release_from_tag(Client *c, uint32_t tag_mask) {
	Monitor *m = c->mon;

	if (!c->isminimized)
		set_minimized(c);

	c->oldtags &= ~tag_mask;
	if (c->oldtags == 0)
		c->oldtags = first_active_tag_mask(m, tag_mask);
}

void dyn_tag_evict_clients(Monitor *m, uint32_t tag_mask, uint32_t *ids,
						   int32_t cap, int32_t *n) {
	Client *c = NULL, *tmp = NULL;

	wl_list_for_each_safe(c, tmp, &server.clients, link) {
		bool hidden_scratchpad, on_tag;

		if (c->mon != m)
			continue;

		hidden_scratchpad = c->is_in_scratchpad && c->isminimized;
		on_tag =
			hidden_scratchpad ? (c->oldtags & tag_mask) : (c->tags & tag_mask);
		if (!on_tag)
			continue;

		if (!c->is_in_scratchpad || c->is_tag_bound) {
			if (ids && n && *n < cap)
				ids[(*n)++] = c->id;
			kill_client(&(Arg){.tc = c, .i = UNFORCE});
		} else {
			scratchpad_release_from_tag(c, tag_mask);
		}
	}
}

void dyn_tag_deactivate(Monitor *m, int32_t tag) {
	uint32_t bit = 1u << (tag - 1);

	m->active_tags_mask &= ~bit;
	m->tags_had_clients_mask &= ~bit;
	m->tags_pending_spawn_mask &= ~bit;
	m->pertag->tag_names[tag][0] = '\0';

	/* the view cannot stay on a tag that no longer exists */
	if (m->tagset[m->seltags] & bit) {
		uint32_t target = first_active_tag_mask(m, 0);
		if (target)
			client_view_on_monitor(&(Arg){.ui = target}, true, m, true);
	}

	if (m->pertag->prevtag == (uint32_t)tag) {
		uint32_t fallback = first_active_tag_mask(m, 0);
		if (fallback)
			m->pertag->prevtag = get_tags_first_tag_num(fallback);
	}

	sync_workspaces_to_tag_num(m);
	arrange(m, false, false);
	printstatus(IPC_WATCH_ARRANGGE);
}

void cleanup_empty_dynamic_tags(Monitor *m) {
	static bool cleaning = false;
	bool changed = false;
	int32_t i;

	if (server.is_migration_active || !config.auto_remove_empty_tags || !m ||
		!m->pertag || m->iscleanuping || cleaning)
		return;

	cleaning = true;

	for (i = config.initial_tags_count + 1; i <= config.tag_num; i++) {
		uint32_t bit = 1u << (i - 1);
		bool has_clients = false;
		Client *c = NULL;

		if (!(m->active_tags_mask & bit))
			continue;

		wl_list_for_each(c, &server.clients, link) {
			if (c->mon == m && !c->isglobal && !c->is_in_scratchpad &&
				(c->tags & bit)) {
				has_clients = true;
				break;
			}
		}

		if (has_clients) {
			m->tags_had_clients_mask |= bit;
			m->tags_pending_spawn_mask &= ~bit;
			continue;
		}

		/* The tag being looked at that never held a client yet stays, so the
		 * first application can still be opened on it. */
		if ((m->tagset[m->seltags] & bit) && !(m->tags_had_clients_mask & bit))
			continue;

		/* a spawn was requested together with the tag: give it time */
		if (m->tags_pending_spawn_mask & bit) {
			if ((uint32_t)(get_now_in_ms() -
						   (uint32_t)m->tag_spawn_time[i - 1]) <
				DYN_TAG_SPAWN_GRACE_MS)
				continue;
			m->tags_pending_spawn_mask &= ~bit;
		}

		/* scratchpads tied to the tag go with it, the others are released */
		dyn_tag_evict_clients(m, bit, NULL, 0, NULL);

		dyn_tag_deactivate(m, i);
		changed = true;
	}

	cleaning = false;

	if (changed)
		printstatus(IPC_WATCH_ARRANGGE);
}

uint32_t dyn_tag_remap_to_monitor(Client *c, Monitor *dest,
								  uint32_t tag_mapping[]) {
	bool hidden_scratchpad = c->is_in_scratchpad && c->isminimized;
	uint32_t oldtags = hidden_scratchpad ? c->oldtags : c->tags;
	uint32_t newtags = 0;
	int32_t bit, k;

	for (bit = 0; bit < config.tag_num; bit++) {
		uint32_t bit_mask = 1u << bit;

		if (!(oldtags & bit_mask))
			continue;

		/* initial tags exist everywhere, they map to themselves */
		if (bit + 1 <= config.initial_tags_count) {
			newtags |= bit_mask;
			continue;
		}

		if (!(dest->active_tags_mask & bit_mask)) {
			/* free on the destination: keep the number */
			newtags |= bit_mask;
			dest->active_tags_mask |= bit_mask;
			continue;
		}

		/* taken on the destination: all the clients of that tag go to the
		 * same free tag */
		if (tag_mapping[bit] != 0) {
			newtags |= tag_mapping[bit];
			continue;
		}

		{
			uint32_t free_mask = 0;
			int32_t free_bit;

			for (free_bit = config.initial_tags_count;
				 free_bit < config.tag_num; free_bit++) {
				uint32_t candidate = 1u << free_bit;
				bool taken = false;

				for (k = 0; k < tag_num_MAX; k++) {
					if (tag_mapping[k] == candidate) {
						taken = true;
						break;
					}
				}
				if (!(dest->active_tags_mask & candidate) && !taken) {
					free_mask = candidate;
					break;
				}
			}

			if (free_mask) {
				tag_mapping[bit] = free_mask;
				newtags |= free_mask;
				dest->active_tags_mask |= free_mask;
			} else {
				/* no room: share the tag the clients already had */
				newtags |= bit_mask;
			}
		}
	}

	return newtags;
}
