#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct ipc_watch_client {
	struct wl_list link;
	int fd;
	struct wl_event_source *source;
	enum ipc_watch_type type;
	union {
		struct {
			char name[64];
		} monitor;
		struct {
			uint32_t id;
		} client;
		struct {
			char mon_name[64];
		} tags;
	} target;
};

static struct wl_list watch_clients;

struct ipc_client_state {
	int fd;
	struct wl_event_source *source;
	struct wl_event_loop *loop;
	char *buf;
	size_t buf_len;
	size_t buf_cap;
};

static void ipc_remove_watch_client(struct ipc_watch_client *wc);
static void ipc_notify_json_to_fd(int fd, cJSON *json);

/* ---------- 工具函数 ---------- */

static Monitor *monitor_by_name(const char *name) {
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (strcmp(m->wlr_output->name, name) == 0)
			return m;
	}
	return NULL;
}

static Client *client_by_id(uint32_t id) {
	Client *c;
	wl_list_for_each(c, &clients, link) {
		if (c->id == id)
			return c;
	}
	return NULL;
}

static const char *ipc_get_layout_str(void) {
	struct wlr_keyboard *keyboard = &kb_group->wlr_group->keyboard;
	xkb_layout_index_t current = xkb_state_serialize_layout(
		keyboard->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
	static char layout[32];
	const char *name = xkb_keymap_layout_get_name(keyboard->keymap, current);
	snprintf(layout, sizeof(layout), "%s", name ? name : "");
	return layout;
}

static cJSON *tags_mask_to_array(uint32_t tagmask) {
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < LENGTH(tags); i++)
		if (tagmask & (1 << i))
			cJSON_AddItemToArray(arr, cJSON_CreateNumber(i + 1));
	return arr;
}

static cJSON *build_tags_json(Monitor *m) {
	cJSON *tags_array = cJSON_CreateArray();
	Client *c = NULL;
	for (int tag = 1; tag <= 32; tag++) {
		if (!(m->active_tags_mask & (1U << (tag - 1))))
			continue;
		int numclients = 0;
		bool is_active = false, is_urgent = false;
		bool is_migrated = false;
		const char *source_monitor = "";
		int source_tag = 0;
		uint32_t tagmask = 1 << (tag - 1);
		if (tagmask & m->tagset[m->seltags])
			is_active = true;
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m)
				continue;
			if (!(c->tags & tagmask & TAGMASK))
				continue;
			if (c->isurgent)
				is_urgent = true;
			if (c->oldmonname[0] != '\0') {
				is_migrated = true;
				source_monitor = c->oldmonname;
				if (c->oldmon_tags) {
					source_tag = get_tags_first_tag_num(c->oldmon_tags);
				}
			}
			numclients++;
		}
		cJSON *tag_obj = cJSON_CreateObject();
		cJSON_AddNumberToObject(tag_obj, "index", tag);
		cJSON_AddBoolToObject(tag_obj, "is_active", is_active);
		cJSON_AddBoolToObject(tag_obj, "is_urgent", is_urgent);
		cJSON_AddBoolToObject(tag_obj, "is_migrated", is_migrated);
		cJSON_AddStringToObject(tag_obj, "source_monitor", source_monitor);
		cJSON_AddNumberToObject(tag_obj, "source_tag", source_tag);
		cJSON_AddStringToObject(tag_obj, "layout",
								m->pertag->ltidxs[tag]->symbol);
		cJSON_AddNumberToObject(tag_obj, "client_count", numclients);
		cJSON_AddItemToArray(tags_array, tag_obj);
	}
	return tags_array;
}

static cJSON *monitor_active_client(Monitor *m) {
	cJSON *obj = cJSON_CreateObject();
	if (!m->sel) {
		cJSON_AddNullToObject(obj, "id");
		cJSON_AddNullToObject(obj, "title");
		cJSON_AddNullToObject(obj, "appid");
		return obj;
	}
	Client *c = m->sel;
	cJSON_AddNumberToObject(obj, "id", c->id);
	cJSON_AddStringToObject(obj, "title", client_get_title(c));
	cJSON_AddStringToObject(obj, "appid", client_get_appid(c));
	return obj;
}

static cJSON *monitor_active_tags(Monitor *m) {
	cJSON *arr = cJSON_CreateArray();
	uint32_t tagset;
	if (m->isoverview) {
		cJSON_AddItemToArray(arr, cJSON_CreateNumber(0));
		return arr;
	}
	tagset = m->tagset[m->seltags];
	for (int i = 0; i < 32; i++)
		if (tagset & (1 << i))
			cJSON_AddItemToArray(arr, cJSON_CreateNumber(i + 1));
	return arr;
}

static cJSON *build_client_json(Client *c) {
	cJSON *obj = cJSON_CreateObject();

	cJSON_AddNumberToObject(obj, "id", c->id);
	cJSON_AddNumberToObject(obj, "pid", c->pid);
	cJSON_AddStringToObject(obj, "title", client_get_title(c));
	cJSON_AddStringToObject(obj, "appid", client_get_appid(c));
	cJSON_AddStringToObject(obj, "mark", c->mark ? c->mark : "");
	cJSON_AddStringToObject(obj, "monitor",
							c->mon ? c->mon->wlr_output->name : "");
	cJSON_AddItemToObject(obj, "tags", tags_mask_to_array(c->tags));
	cJSON_AddBoolToObject(obj, "is_focused", c->isfocusing);
	cJSON_AddBoolToObject(obj, "is_fullscreen", c->isfullscreen);
	cJSON_AddBoolToObject(obj, "is_floating", c->isfloating);
	cJSON_AddBoolToObject(obj, "is_maximized", c->ismaximizescreen);
	cJSON_AddBoolToObject(obj, "is_global", c->isglobal);
	cJSON_AddBoolToObject(obj, "is_unglobal", c->isunglobal);
	cJSON_AddBoolToObject(obj, "is_overlay", c->isoverlay);
	cJSON_AddBoolToObject(obj, "is_fakefullscreen", c->isfakefullscreen);
	cJSON_AddBoolToObject(obj, "is_minimized", c->isminimized);
	cJSON_AddBoolToObject(obj, "is_urgent", c->isurgent);
	cJSON_AddBoolToObject(obj, "is_scratchpad", c->is_in_scratchpad);
	cJSON_AddBoolToObject(obj, "is_namedscratchpad", c->isnamedscratchpad);
	cJSON_AddBoolToObject(obj, "is_migrated", c->oldmonname[0] != '\0');
	cJSON_AddStringToObject(obj, "source_monitor", c->oldmonname);
	cJSON_AddItemToObject(obj, "source_tags", tags_mask_to_array(c->oldmon_tags));
	cJSON_AddNumberToObject(obj, "x", c->geom.x);
	cJSON_AddNumberToObject(obj, "y", c->geom.y);
	cJSON_AddNumberToObject(obj, "width", c->geom.width);
	cJSON_AddNumberToObject(obj, "height", c->geom.height);
	return obj;
}

static cJSON *build_monitor_json(Monitor *m) {
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddStringToObject(resp, "name", m->wlr_output->name);
	cJSON_AddBoolToObject(resp, "active", m == selmon);
	cJSON_AddNumberToObject(resp, "x", m->m.x);
	cJSON_AddNumberToObject(resp, "y", m->m.y);
	cJSON_AddNumberToObject(resp, "width", m->m.width);
	cJSON_AddNumberToObject(resp, "height", m->m.height);
	cJSON_AddNumberToObject(resp, "scale", m->wlr_output->scale);
	cJSON_AddNumberToObject(resp, "layout_index",
							m->pertag->ltidxs[m->pertag->curtag] - layouts);
	cJSON_AddStringToObject(resp, "layout_symbol",
							m->pertag->ltidxs[m->pertag->curtag]->symbol);
	cJSON_AddStringToObject(resp, "last_open_surface", m->last_open_surface);
	cJSON_AddItemToObject(resp, "tags", build_tags_json(m));
	cJSON_AddItemToObject(resp, "active_tags", monitor_active_tags(m));
	cJSON_AddItemToObject(resp, "active_client", monitor_active_client(m));
	cJSON_AddItemToObject(resp, "keymode", cJSON_CreateString(keymode.mode));
	cJSON_AddItemToObject(resp, "keyboardlayout",
						  cJSON_CreateString(ipc_get_layout_str()));
	return resp;
}

static cJSON *build_all_tags_entry(Monitor *m) {
	cJSON *entry = cJSON_CreateObject();
	cJSON_AddStringToObject(entry, "monitor", m->wlr_output->name);
	cJSON_AddItemToObject(entry, "tags", build_tags_json(m));
	return entry;
}

static cJSON *build_all_tags_response(void) {
	cJSON *arr = cJSON_CreateArray();
	Monitor *m;
	wl_list_for_each(m, &mons, link)
		cJSON_AddItemToArray(arr, build_all_tags_entry(m));
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddItemToObject(resp, "all_tags", arr);
	return resp;
}

static cJSON *build_monitor_tags_response(Monitor *m) {
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddStringToObject(resp, "monitor", m->wlr_output->name);
	cJSON_AddItemToObject(resp, "tags", build_tags_json(m));
	cJSON_AddItemToObject(resp, "active_tags", monitor_active_tags(m));
	return resp;
}

static void send_static_json(int fd, const char *json_str) {
	size_t len = strlen(json_str);
	send(fd, json_str, len, 0);
}

/* ---------- 一次性命令处理 ---------- */
static void handle_command(int client_fd, const char *cmd_raw) {
	cJSON *resp = NULL;
	char *json_str = NULL;
	char cmd[1024];

	strncpy(cmd, cmd_raw, sizeof(cmd) - 1);
	cmd[sizeof(cmd) - 1] = '\0';
	for (char *p = cmd; *p; p++)
		if (*p == ',')
			*p = ' ';

	if (strcmp(cmd, "get version") == 0) {
		resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "version", VERSION);
	} else if (strcmp(cmd, "get keymode") == 0) {
		resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "keymode", keymode.mode);
	} else if (strcmp(cmd, "get keyboardlayout") == 0) {
		resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "layout", ipc_get_layout_str());
	} else if (strcmp(cmd, "get last_open_surface") == 0 ||
			   strncmp(cmd, "get last_open_surface ", 22) == 0) {
		Monitor *m;
		if (cmd[21] == '\0') { // exactly "get last_open_surface"
			m = selmon;
		} else {
			m = monitor_by_name(cmd + 22);
		}
		if (!m) {
			send_static_json(client_fd, "{\"error\":\"monitor not found\"}\n");
			return;
		}
		resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "monitor", m->wlr_output->name);
		cJSON_AddStringToObject(resp, "last_open_surface",
								m->last_open_surface);
	} else if (strncmp(cmd, "get monitor ", 12) == 0) {
		Monitor *m = monitor_by_name(cmd + 12);
		if (!m) {
			send_static_json(client_fd, "{\"error\":\"monitor not found\"}\n");
			return;
		}
		resp = build_monitor_json(m);
	} else if (strcmp(cmd, "get focusing-client") == 0) {
		if (selmon && selmon->sel) {
			resp = build_client_json(selmon->sel);
		} else {
			send_static_json(client_fd, "{\"error\":\"no focused client\"}\n");
			return;
		}
	} else if (strncmp(cmd, "get client_by_mark ", 19) == 0) {
		char mark_name[256] = "";
		if (sscanf(cmd + 19, "%255s", mark_name) != 1) {
			send_static_json(client_fd, "{\"error\":\"usage: get client_by_mark <mark>\"}\n");
			return;
		}
		Client *target = NULL;
		Client *c = NULL;
		wl_list_for_each(c, &clients, link) {
			if (c->mark && strcmp(c->mark, mark_name) == 0) {
				target = c;
				break;
			}
		}
		if (!target) {
			send_static_json(client_fd, "{\"error\":\"client not found\"}\n");
			return;
		}
		resp = build_client_json(target);
	} else if (strncmp(cmd, "get client ", 11) == 0) {
		Client *c = client_by_id((uint32_t)atoi(cmd + 11));
		if (!c) {
			send_static_json(client_fd, "{\"error\":\"client not found\"}\n");
			return;
		}
		resp = build_client_json(c);
	} else if (strncmp(cmd, "get tag ", 8) == 0) {
		char mon_name[64];
		int ext_tag_idx;
		if (sscanf(cmd + 8, "%63s %d", mon_name, &ext_tag_idx) != 2) {
			send_static_json(
				client_fd,
				"{\"error\":\"usage: get tag <monitor> <index>\"}\n");
			return;
		}
		int tag_idx = ext_tag_idx - 1;
		Monitor *m = monitor_by_name(mon_name);
		if (!m || tag_idx < 0 || tag_idx >= 32 || !(m->active_tags_mask & (1U << tag_idx))) {
			send_static_json(client_fd,
							 "{\"error\":\"invalid monitor or tag index\"}\n");
			return;
		}
		uint32_t tagmask = 1 << tag_idx;
		int numclients = 0, focused_client = 0;
		bool is_active = false, is_urgent = false;
		if (tagmask & m->tagset[m->seltags])
			is_active = true;

		Client *c, *focused = focustop(m);
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m || !(c->tags & tagmask))
				continue;
			if (c == focused)
				focused_client = 1;
			if (c->isurgent)
				is_urgent = true;
			numclients++;
		}
		resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "monitor", m->wlr_output->name);
		cJSON_AddNumberToObject(resp, "tag_index", ext_tag_idx);
		cJSON_AddBoolToObject(resp, "is_active", is_active);
		cJSON_AddBoolToObject(resp, "is_urgent", is_urgent);
		cJSON_AddNumberToObject(resp, "client_count", numclients);
		cJSON_AddBoolToObject(resp, "focused_client", focused_client);
	} else if (strcmp(cmd, "get all-clients") == 0) {
		cJSON *arr = cJSON_CreateArray();
		Client *c;
		wl_list_for_each(c, &clients, link)
			cJSON_AddItemToArray(arr, build_client_json(c));
		resp = cJSON_CreateObject();
		cJSON_AddItemToObject(resp, "clients", arr);
	} else if (strcmp(cmd, "get all-monitors") == 0) {
		cJSON *arr = cJSON_CreateArray();
		Monitor *m;
		wl_list_for_each(m, &mons, link)
			cJSON_AddItemToArray(arr, build_monitor_json(m));
		resp = cJSON_CreateObject();
		cJSON_AddItemToObject(resp, "monitors", arr);
	} else if (strcmp(cmd, "get all-tags") == 0) {
		resp = build_all_tags_response();
	} else if (strncmp(cmd, "add_tag", 7) == 0) {
		if (!selmon) {
			send_static_json(client_fd, "{\"error\":\"no focused monitor\"}\n");
			return;
		}

		int new_tag_idx = -1;
		for (int i = 0; i < 32; i++) {
			if (!(selmon->active_tags_mask & (1U << i))) {
				new_tag_idx = i + 1;
				break;
			}
		}

		if (new_tag_idx == -1) {
			send_static_json(client_fd, "{\"error\":\"max tags limit reached\"}\n");
			return;
		}

		char *spawn_cmd = NULL;
		if (strlen(cmd) > 7 && cmd[7] == ' ') {
			spawn_cmd = cmd + 8;
			while (*spawn_cmd == ' ') {
				spawn_cmd++;
			}
			if (strlen(spawn_cmd) == 0) {
				spawn_cmd = NULL;
			}
		}

		selmon->active_tags_mask |= (1U << (new_tag_idx - 1));
		refresh_monitors_workspaces_status(selmon);

		view(&(Arg){.ui = 1U << (new_tag_idx - 1)}, true);

		arrange(selmon, false, false);
		printstatus(IPC_WATCH_ARRANGGE);

		if (spawn_cmd) {
			spawn_shell(&(Arg){.v = spawn_cmd});
		}

		resp = cJSON_CreateObject();
		cJSON_AddBoolToObject(resp, "success", true);
		cJSON_AddNumberToObject(resp, "index", new_tag_idx);
		cJSON_AddStringToObject(resp, "monitor", selmon->wlr_output->name);
	} else if (strncmp(cmd, "move_tag ", 9) == 0) {
		int tag_idx = -1;
		char dest_mon_name[256] = "";

		if (sscanf(cmd + 9, "%d %255s", &tag_idx, dest_mon_name) != 2) {
			send_static_json(client_fd, "{\"error\":\"usage: move_tag <tag_index> <destination_monitor>\"}\n");
			return;
		}

		if (tag_idx <= config.initial_tags_count || tag_idx > 32) {
			char err_msg[256];
			snprintf(err_msg, sizeof(err_msg), "{\"error\":\"tag index must be a dynamic tag (greater than %d and <= 32)\"}\n", config.initial_tags_count);
			send_static_json(client_fd, err_msg);
			return;
		}

		// 1. Buscar monitor destino
		Monitor *dest_mon = NULL;
		Monitor *m;
		wl_list_for_each(m, &mons, link) {
			if (strcmp(m->wlr_output->name, dest_mon_name) == 0) {
				dest_mon = m;
				break;
			}
		}
		if (!dest_mon || !dest_mon->wlr_output->enabled) {
			send_static_json(client_fd, "{\"error\":\"destination monitor not found or disabled\"}\n");
			return;
		}

		// 2. Buscar monitor origen que tenga activo ese tag
		Monitor *src_mon = NULL;
		wl_list_for_each(m, &mons, link) {
			if (m->wlr_output->enabled && (m->active_tags_mask & (1U << (tag_idx - 1)))) {
				src_mon = m;
				break;
			}
		}
		if (!src_mon) {
			send_static_json(client_fd, "{\"error\":\"tag index is not active on any monitor\"}\n");
			return;
		}

		if (src_mon == dest_mon) {
			send_static_json(client_fd, "{\"error\":\"tag is already on the destination monitor\"}\n");
			return;
		}

		// 3. Determinar tag de destino en dest_mon
		uint32_t src_tag_mask = 1U << (tag_idx - 1);
		uint32_t dest_tag_mask = 0;
		int dest_tag_idx = -1;

		if (!(dest_mon->active_tags_mask & src_tag_mask)) {
			dest_tag_mask = src_tag_mask;
			dest_tag_idx = tag_idx;
		} else {
			for (int i = config.initial_tags_count; i < 32; i++) {
				uint32_t candidate_mask = 1U << i;
				if (!(dest_mon->active_tags_mask & candidate_mask)) {
					dest_tag_mask = candidate_mask;
					dest_tag_idx = i + 1;
					break;
				}
			}
		}

		if (dest_tag_mask == 0) {
			send_static_json(client_fd, "{\"error\":\"no free dynamic tags available on destination monitor\"}\n");
			return;
		}

		// 4. Mover clientes del tag en src_mon al dest_mon
		dest_mon->active_tags_mask |= dest_tag_mask;
		is_migration_active = true;

		Client *c;
		wl_list_for_each(c, &clients, link) {
			if (c->mon == src_mon) {
				bool is_target = false;
				if (!c->is_in_scratchpad && (c->tags & src_tag_mask)) {
					is_target = true;
				} else if (c->is_in_scratchpad && c->is_tag_bound) {
					if ((c->is_scratchpad_show && (c->tags & src_tag_mask)) ||
						(!c->is_scratchpad_show && (c->mini_restore_tag & src_tag_mask))) {
						is_target = true;
					}
				}

				if (is_target) {
					// Guardar historial para reporte de migración
					memset(c->oldmonname, 0, sizeof(c->oldmonname));
					strncpy(c->oldmonname, src_mon->wlr_output->name, sizeof(c->oldmonname) - 1);
					c->oldmon_tags = src_tag_mask;

					// Si es un scratchpad, actualizar su tag atado al nuevo tag de destino y recalcular coordenadas
					if (c->is_in_scratchpad) {
						if (c->is_scratchpad_show) {
							c->tags = dest_tag_mask;
						} else {
							c->mini_restore_tag = dest_tag_mask;
							c->oldtags = dest_tag_mask;
						}
						c->float_geom.width = (int32_t)(c->float_geom.width * dest_mon->w.width / src_mon->w.width);
						c->float_geom.height = (int32_t)(c->float_geom.height * dest_mon->w.height / src_mon->w.height);
						c->float_geom = c->geom = setclient_coordinate_center(c, dest_mon, c->float_geom, 0, 0);
					}

					// Cambiar de monitor
					setmon(c, dest_mon, dest_tag_mask, true);
				}
			}
		}

		// 5. Limpieza y refresco
		is_migration_active = false;
		cleanup_empty_dynamic_tags(src_mon);
		arrange(src_mon, false, false);
		arrange(dest_mon, false, false);
		printstatus(IPC_WATCH_ARRANGGE);

		resp = cJSON_CreateObject();
		cJSON_AddBoolToObject(resp, "success", true);
		cJSON_AddStringToObject(resp, "source_monitor", src_mon->wlr_output->name);
		cJSON_AddStringToObject(resp, "destination_monitor", dest_mon->wlr_output->name);
		cJSON_AddNumberToObject(resp, "source_tag", tag_idx);
		cJSON_AddNumberToObject(resp, "destination_tag", dest_tag_idx);
	} else if (strncmp(cmd, "viewmon ", 8) == 0) {
		char mon_name[256] = "";
		int tag_idx = -1;
		if (sscanf(cmd + 8, "%255s %d", mon_name, &tag_idx) == 2) {
      if (tag_idx < 1 || tag_idx > 32) {
				send_static_json(client_fd, "{\"error\":\"invalid tag index, must be between 1 and 32\"}\n");
				return;
			}
			Monitor *m;
			Monitor *target_mon = NULL;
			wl_list_for_each(m, &mons, link) {
				if (strcmp(m->wlr_output->name, mon_name) == 0) {
					target_mon = m;
					break;
				}
			}
			if (target_mon) {
				focusmon(&(Arg){.v = mon_name, .i = UNDIR});
				view(&(Arg){.ui = 1U << (tag_idx - 1)}, true);
				resp = cJSON_CreateObject();
				cJSON_AddBoolToObject(resp, "success", true);
			} else {
				send_static_json(client_fd, "{\"error\":\"monitor not found\"}\n");
				return;
			}
		} else {
			send_static_json(client_fd, "{\"error\":\"usage: viewmon <monitor> <tag>\"}\n");
			return;
		}
	} else if (strncmp(cmd, "remove_tag", 10) == 0) {
		if (!selmon) {
			send_static_json(client_fd, "{\"error\":\"no focused monitor\"}\n");
			return;
		}

		int tag_idx = -1;
		char force_str[32] = "";
		bool force_kill = false;

		int parsed = sscanf(cmd + 10, "%d %31s", &tag_idx, force_str);
		if (parsed == 2) {
			if (strcmp(force_str, "force") == 0 || strcmp(force_str, "1") == 0 || strcmp(force_str, "true") == 0) {
				force_kill = true;
			}
		} else if (parsed == 1) {
			char arg1[32] = "";
			if (sscanf(cmd + 10, "%31s", arg1) == 1) {
				if (strcmp(arg1, "force") == 0 || strcmp(arg1, "1") == 0 || strcmp(arg1, "true") == 0) {
					force_kill = true;
					tag_idx = -1;
				}
			}
		}

		if (tag_idx == -1) {
			for (int i = 31; i >= 0; i--) {
				if (selmon->active_tags_mask & (1U << i)) {
					tag_idx = i + 1;
					break;
				}
			}
		}

		if (tag_idx < 1 || tag_idx > 32 || !(selmon->active_tags_mask & (1U << (tag_idx - 1)))) {
			send_static_json(client_fd, "{\"error\":\"invalid or inactive tag index\"}\n");
			return;
		}

		int active_count = 0;
		for (int i = 0; i < 32; i++) {
			if (selmon->active_tags_mask & (1U << i)) {
				active_count++;
			}
		}
		if (active_count <= 1) {
			send_static_json(client_fd, "{\"error\":\"cannot remove the last active tag\"}\n");
			return;
		}

		uint32_t tag_mask = 1U << (tag_idx - 1);

		bool has_clients = false;
		Client *c;
		wl_list_for_each(c, &clients, link) {
			if (c->mon == selmon) {
				if (c->tags & tag_mask) {
					if (!c->is_in_scratchpad || c->is_tag_bound) {
						has_clients = true;
						break;
					}
				} else if (c->is_in_scratchpad && !c->is_scratchpad_show && c->is_tag_bound && (c->mini_restore_tag & tag_mask)) {
					has_clients = true;
					break;
				}
			}
		}

		resp = cJSON_CreateObject();
		cJSON_AddBoolToObject(resp, "success", true);
		cJSON_AddNumberToObject(resp, "removed_tag", tag_idx);
		cJSON_AddNumberToObject(resp, "tag_mask", tag_mask);
		cJSON_AddNumberToObject(resp, "active_tags_mask_before", selmon->active_tags_mask);

		cJSON *killed_arr = cJSON_CreateArray();
		if (has_clients) {
			if (!force_kill) {
				cJSON_Delete(resp);
				send_static_json(client_fd, "{\"error\":\"tag has active clients\"}\n");
				return;
			} else {
				Client *tmp;
				wl_list_for_each_safe(c, tmp, &clients, link) {
					if (c->mon == selmon) {
						if (c->tags & tag_mask) {
							if (!c->is_in_scratchpad || c->is_tag_bound) {
								cJSON_AddItemToArray(killed_arr, cJSON_CreateNumber(c->id));
								killclient(&(Arg){.tc = c, .i = UNFORCE});
							} else {
								set_minimized(c);
								c->mini_restore_tag &= ~tag_mask;
								if (c->mini_restore_tag == 0) {
									c->mini_restore_tag = selmon->tagset[selmon->seltags];
								}
								c->oldtags &= ~tag_mask;
								if (c->oldtags == 0) {
									c->oldtags = selmon->tagset[selmon->seltags];
								}
							}
						} else if (c->is_in_scratchpad && !c->is_scratchpad_show && (c->mini_restore_tag & tag_mask)) {
							if (c->is_tag_bound) {
								cJSON_AddItemToArray(killed_arr, cJSON_CreateNumber(c->id));
								killclient(&(Arg){.tc = c, .i = UNFORCE});
							} else {
								c->mini_restore_tag &= ~tag_mask;
								if (c->mini_restore_tag == 0) {
									c->mini_restore_tag = selmon->tagset[selmon->seltags];
								}
								c->oldtags &= ~tag_mask;
								if (c->oldtags == 0) {
									c->oldtags = selmon->tagset[selmon->seltags];
								}
							}
						}
					}
				}
			}
		}
		cJSON_AddItemToObject(resp, "killed_clients", killed_arr);

		selmon->active_tags_mask &= ~tag_mask;
		selmon->tags_had_clients_mask &= ~tag_mask;
		cJSON_AddNumberToObject(resp, "active_tags_mask_after", selmon->active_tags_mask);

		if (selmon->tagset[selmon->seltags] & tag_mask) {
			selmon->tagset[selmon->seltags] &= ~tag_mask;
			if (selmon->tagset[selmon->seltags] == 0) {
				for (int i = 0; i < 32; i++) {
					if (selmon->active_tags_mask & (1U << i)) {
						view(&(Arg){.ui = 1U << i}, true);
						break;
					}
				}
			}
		}

		if (selmon->pertag->prevtag == tag_idx) {
			for (int i = 0; i < 32; i++) {
				if (selmon->active_tags_mask & (1U << i)) {
					selmon->pertag->prevtag = i + 1;
					break;
				}
			}
		}

		refresh_monitors_workspaces_status(selmon);
		arrange(selmon, false, false);
		printstatus(IPC_WATCH_ARRANGGE);
	} else if (strncmp(cmd, "get tags ", 9) == 0) {
		Monitor *m = monitor_by_name(cmd + 9);
		if (!m) {
			send_static_json(client_fd, "{\"error\":\"monitor not found\"}\n");
			return;
		}
		resp = build_monitor_tags_response(m);
	} else if (strncmp(cmd, "mark_client ", 12) == 0) {
		uint32_t client_id = 0;
		char mark_name[256] = "";
		if (sscanf(cmd + 12, "%u %255s", &client_id, mark_name) != 2) {
			send_static_json(client_fd, "{\"error\":\"usage: mark_client <id> <mark>\"}\n");
			return;
		}
		Client *c = client_by_id(client_id);
		if (!c) {
			send_static_json(client_fd, "{\"error\":\"client not found\"}\n");
			return;
		}
		free(c->mark);
		if (strcmp(mark_name, "none") == 0) {
			c->mark = NULL;
		} else {
			c->mark = strdup(mark_name);
		}

		resp = cJSON_CreateObject();
		cJSON_AddBoolToObject(resp, "success", true);
	} else if (strncmp(cmd, "dispatch ", 9) == 0) {
		char *dispatch_copy = strdup(cmd_raw + 9);
		char *out = dispatch_copy, *ptr = dispatch_copy;
		int client_id = -1;
		while (*ptr) {
			while (*ptr == ' ' || *ptr == '\t')
				*out++ = *ptr++;
			if (strncmp(ptr, "client,", 7) == 0) {
				char *end;
				long id = strtol(ptr + 7, &end, 10);
				if (id > 0 && end > ptr + 7 && (*end == '\0' || *end == ',')) {
					client_id = (int)id;
					ptr = end;
					if (*ptr == ',')
						ptr++;
					continue;
				}
			}
			*out++ = *ptr++;
		}
		*out = '\0';

		char *tokens[6] = {NULL};
		int token_count = 0;
		char *saveptr;
		char *token = strtok_r(dispatch_copy, ",", &saveptr);
		while (token && token_count < 6) {
			while (*token == ' ' || *token == '\t')
				token++;
			char *end = token + strlen(token) - 1;
			while (end >= token && (*end == ' ' || *end == '\t'))
				*end-- = '\0';
			tokens[token_count++] = token;
			token = strtok_r(NULL, ",", &saveptr);
		}

		Arg arg = {0};
		int32_t (*func)(const Arg *) = parse_func_name(
			token_count > 0 ? tokens[0] : "", &arg,
			token_count > 1 ? tokens[1] : "", token_count > 2 ? tokens[2] : "",
			token_count > 3 ? tokens[3] : "", token_count > 4 ? tokens[4] : "",
			token_count > 5 ? tokens[5] : "");

		if (func && client_id > 0)
			arg.tc = client_by_id((uint32_t)client_id);

		if (func) {
			func(&arg);
			send_static_json(client_fd, "{\"success\":true}\n");
		} else {
			send_static_json(client_fd, "{\"error\":\"unknown function\"}\n");
		}

		if (arg.v)
			free(arg.v);
		if (arg.v2)
			free(arg.v2);
		if (arg.v3)
			free(arg.v3);
		free(dispatch_copy);
		return; // Fast path exit
	} else {
		send_static_json(client_fd, "{\"error\":\"unknown command\"}\n");
		return;
	}

	if (resp) {
		json_str = cJSON_PrintUnformatted(resp);
		if (json_str) {
			size_t len = strlen(json_str);
			char *msg = malloc(len + 2);
			if (msg) {
				snprintf(msg, len + 2, "%s\n", json_str);
				send(client_fd, msg, len + 1, 0);
				free(msg);
			}
			free(json_str);
		}
		cJSON_Delete(resp);
	}
}

/* ---------- Watch 模式支持 ---------- */
static void ipc_notify_json_to_fd(int fd, cJSON *json) {
	char *str = cJSON_PrintUnformatted(json);
	if (!str)
		return;
	size_t len = strlen(str);
	char *msg = malloc(len + 2);
	if (!msg) {
		free(str);
		return;
	}
	snprintf(msg, len + 2, "%s\n", str);
	if (send(fd, msg, len + 1, 0) < 0) {
		struct ipc_watch_client *wc, *tmp;
		wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
			if (wc->fd == fd) {
				ipc_remove_watch_client(wc);
				break;
			}
		}
	}
	free(msg);
	free(str);
}

static void ipc_remove_watch_client(struct ipc_watch_client *wc) {
	wl_list_remove(&wc->link);
	wl_event_source_remove(wc->source);
	close(wc->fd);
	free(wc);
}

static int ipc_watch_data_handler(int fd, uint32_t mask, void *data) {
	struct ipc_watch_client *wc = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		ipc_remove_watch_client(wc);
		return 0;
	}
	if (mask & WL_EVENT_READABLE) {
		char buf[64];
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
			ipc_remove_watch_client(wc);
		}
	}
	return 0;
}

static bool handle_watch_command(int fd, const char *cmd,
								 struct ipc_client_state *client) {
	enum ipc_watch_type type = IPC_WATCH_NONE;
	const char *arg = NULL;
	uint32_t client_id = 0;

	if (strncmp(cmd, "watch monitor ", 14) == 0) {
		type = IPC_WATCH_MONITOR;
		arg = cmd + 14;
	} else if (strcmp(cmd, "watch focusing-client") == 0) {
		type = IPC_WATCH_FOCUSING_CLIENT;
	} else if (strncmp(cmd, "watch client ", 13) == 0) {
		type = IPC_WATCH_CLIENT;
		client_id = (uint32_t)atoi(cmd + 13);
	} else if (strncmp(cmd, "watch tags ", 11) == 0) {
		type = IPC_WATCH_TAGS;
		arg = cmd + 11;
	} else if (strcmp(cmd, "watch all-monitors") == 0) {
		type = IPC_WATCH_ALL_MONITORS;
	} else if (strcmp(cmd, "watch all-tags") == 0) {
		type = IPC_WATCH_ALL_TAGS;
	} else if (strcmp(cmd, "watch all-clients") == 0) {
		type = IPC_WATCH_ALL_CLIENTS;
	} else if (strcmp(cmd, "watch keymode") == 0) {
		type = IPC_WATCH_KEYMODE;
	} else if (strcmp(cmd, "watch keyboardlayout") == 0) {
		type = IPC_WATCH_KB_LAYOUT;
	} else if (strcmp(cmd, "watch last_open_surface") == 0 ||
			   strncmp(cmd, "watch last_open_surface ", 24) == 0) {
		type = IPC_WATCH_LAST_OPEN_SURFACE;
		if (cmd[24] != '\0') { // has argument after the space
			arg = cmd + 24;
		} else {
			arg = NULL; // default to selmon
		}
	}

	if (type == IPC_WATCH_NONE)
		return false;

	struct ipc_watch_client *wc = calloc(1, sizeof(*wc));
	wc->fd = fd;
	wc->type = type;

	if ((type == IPC_WATCH_MONITOR || type == IPC_WATCH_LAST_OPEN_SURFACE) &&
		arg)
		snprintf(wc->target.monitor.name, sizeof(wc->target.monitor.name), "%s",
				 arg);
	else if (type == IPC_WATCH_TAGS && arg)
		snprintf(wc->target.tags.mon_name, sizeof(wc->target.tags.mon_name),
				 "%s", arg);
	else if (type == IPC_WATCH_CLIENT)
		wc->target.client.id = client_id;

	wl_event_source_remove(client->source);
	wc->source = wl_event_loop_add_fd(
		client->loop, fd, WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
		ipc_watch_data_handler, wc);
	wl_list_insert(&watch_clients, &wc->link);

	/* 推送初始状态 */
	cJSON *json = NULL;
	switch (type) {
	case IPC_WATCH_MONITOR: {
		Monitor *m = monitor_by_name(arg);
		if (m)
			json = build_monitor_json(m);
		break;
	}
	case IPC_WATCH_LAST_OPEN_SURFACE: {
		Monitor *m = NULL;
		if (arg) {
			m = monitor_by_name(arg);
		} else {
			m = selmon;
		}
		if (m) {
			json = cJSON_CreateObject();
			cJSON_AddStringToObject(json, "monitor", m->wlr_output->name);
			cJSON_AddStringToObject(json, "last_open_surface",
									m->last_open_surface);
		}
		break;
	}
	case IPC_WATCH_FOCUSING_CLIENT: {
		if (selmon && selmon->sel) {
			json = build_client_json(selmon->sel);
		} else {
			json = cJSON_CreateObject();
			cJSON_AddNullToObject(json, "id");
			cJSON_AddNullToObject(json, "title");
			cJSON_AddNullToObject(json, "appid");
		}
		break;
	}
	case IPC_WATCH_CLIENT: {
		Client *c = client_by_id(client_id);
		if (c)
			json = build_client_json(c);
		break;
	}
	case IPC_WATCH_TAGS: {
		Monitor *m = monitor_by_name(arg);
		if (m)
			json = build_monitor_tags_response(m);
		break;
	}
	case IPC_WATCH_ALL_MONITORS: {
		cJSON *arr = cJSON_CreateArray();
		Monitor *m;
		wl_list_for_each(m, &mons, link)
			cJSON_AddItemToArray(arr, build_monitor_json(m));
		json = cJSON_CreateObject();
		cJSON_AddItemToObject(json, "monitors", arr);
		break;
	}
	case IPC_WATCH_ALL_TAGS: {
		json = build_all_tags_response();
		break;
	}
	case IPC_WATCH_ALL_CLIENTS: {
		cJSON *arr = cJSON_CreateArray();
		Client *c;
		wl_list_for_each(c, &clients, link)
			cJSON_AddItemToArray(arr, build_client_json(c));
		json = cJSON_CreateObject();
		cJSON_AddItemToObject(json, "clients", arr);
		break;
	}
	case IPC_WATCH_KEYMODE: {
		json = cJSON_CreateObject();
		cJSON_AddStringToObject(json, "keymode", keymode.mode);
		break;
	}
	case IPC_WATCH_KB_LAYOUT: {
		json = cJSON_CreateObject();
		cJSON_AddStringToObject(json, "layout", ipc_get_layout_str());
		break;
	}
	default:
		break;
	}

	if (json) {
		ipc_notify_json_to_fd(fd, json);
		cJSON_Delete(json);
	}

	free(client->buf);
	free(client);
	return true;
}

/* ---------- Socket 事件处理 ---------- */
static int ipc_handle_client_data(int fd, uint32_t mask, void *data) {
	struct ipc_client_state *client = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
		goto cleanup;

	if (mask & WL_EVENT_READABLE) {
		size_t available = client->buf_cap - client->buf_len;
		if (available < 4096) {
			size_t new_cap = client->buf_cap ? client->buf_cap * 2 : 8192;
			char *new_buf = realloc(client->buf, new_cap);
			if (!new_buf) {
				wlr_log(WLR_ERROR, "IPC: out of memory");
				goto cleanup;
			}
			client->buf = new_buf;
			client->buf_cap = new_cap;
			available = client->buf_cap - client->buf_len;
		}

		ssize_t n = recv(fd, client->buf + client->buf_len, available - 1, 0);
		if (n <= 0)
			goto cleanup;

		client->buf_len += n;
		client->buf[client->buf_len] = '\0';

		char *nl = memchr(client->buf, '\n', client->buf_len);
		if (!nl) {
			if (client->buf_len > 1024 * 1024)
				goto cleanup;
			return 0;
		}
		*nl = '\0';
		char *cmd = client->buf;

		bool is_watch = handle_watch_command(fd, cmd, client);
		if (is_watch)
			return 0;

		handle_command(fd, cmd);
		goto cleanup;
	}
	return 0;

cleanup:
	close(client->fd);
	wl_event_source_remove(client->source);
	free(client->buf);
	free(client);
	return 0;
}

static int ipc_handle_connection(int fd, uint32_t mask, void *data) {
	struct wl_event_loop *loop = data;
	int client_fd = accept(fd, NULL, NULL);
	if (client_fd < 0)
		return 0;

	// 设置 O_NONBLOCK
	int flags = fcntl(client_fd, F_GETFL, 0);
	fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
	// 设置 FD_CLOEXEC
	flags = fcntl(client_fd, F_GETFD, 0);
	fcntl(client_fd, F_SETFD, flags | FD_CLOEXEC);

	struct ipc_client_state *client = calloc(1, sizeof(*client));
	client->fd = client_fd;
	client->loop = loop;
	client->source = wl_event_loop_add_fd(
		loop, client_fd, WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
		ipc_handle_client_data, client);
	return 0;
}

/* ---------- 外部通知接口 ---------- */

void ipc_notify_monitor(Monitor *m) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_MONITOR &&
			strcmp(m->wlr_output->name, wc->target.monitor.name) == 0) {
			if (!json_str) {
				cJSON *json = build_monitor_json(m);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			if (send(wc->fd, json_str, len + 1, 0) < 0)
				ipc_remove_watch_client(wc);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_last_surface_ws_name(Monitor *m) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type != IPC_WATCH_LAST_OPEN_SURFACE)
			continue;

		bool match = false;
		if (wc->target.monitor.name[0] == '\0') {
			if (m == selmon)
				match = true;
		} else {
			if (strcmp(m->wlr_output->name, wc->target.monitor.name) == 0)
				match = true;
		}

		if (!match)
			continue;

		if (!json_str) {
			cJSON *json = cJSON_CreateObject();
			cJSON_AddStringToObject(json, "monitor", m->wlr_output->name);
			cJSON_AddStringToObject(json, "last_open_surface",
									m->last_open_surface);
			char *raw = cJSON_PrintUnformatted(json);
			cJSON_Delete(json);
			if (!raw)
				return;
			len = strlen(raw);
			json_str = malloc(len + 2);
			snprintf(json_str, len + 2, "%s\n", raw);
			free(raw);
		}
		if (send(wc->fd, json_str, len + 1, 0) < 0)
			ipc_remove_watch_client(wc);
	}
	free(json_str);
}

void ipc_notify_focusing_client(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_FOCUSING_CLIENT) {
			if (!json_str) {
				cJSON *json = NULL;
				if (selmon && selmon->sel) {
					json = build_client_json(selmon->sel);
				} else {
					json = cJSON_CreateObject();
					cJSON_AddNullToObject(json, "id");
					cJSON_AddNullToObject(json, "title");
					cJSON_AddNullToObject(json, "appid");
				}
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			if (send(wc->fd, json_str, len + 1, 0) < 0)
				ipc_remove_watch_client(wc);
		}
	}
	free(json_str);
}

void ipc_notify_client(Client *c) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_CLIENT && c->id == wc->target.client.id) {
			if (!json_str) {
				cJSON *json = build_client_json(c);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			if (send(wc->fd, json_str, len + 1, 0) < 0)
				ipc_remove_watch_client(wc);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_tags(Monitor *m) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_TAGS &&
			strcmp(m->wlr_output->name, wc->target.tags.mon_name) == 0) {
			if (!json_str) {
				cJSON *json = build_monitor_tags_response(m);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			if (send(wc->fd, json_str, len + 1, 0) < 0)
				ipc_remove_watch_client(wc);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_all_monitors(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_ALL_MONITORS) {
			if (!json_str) {
				cJSON *arr = cJSON_CreateArray();
				Monitor *m;
				wl_list_for_each(m, &mons, link)
					cJSON_AddItemToArray(arr, build_monitor_json(m));
				cJSON *json = cJSON_CreateObject();
				cJSON_AddItemToObject(json, "monitors", arr);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			if (send(wc->fd, json_str, len + 1, 0) < 0)
				ipc_remove_watch_client(wc);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_all_clients(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_ALL_CLIENTS) {
			if (!json_str) {
				cJSON *arr = cJSON_CreateArray();
				Client *c;
				wl_list_for_each(c, &clients, link)
					cJSON_AddItemToArray(arr, build_client_json(c));
				cJSON *json = cJSON_CreateObject();
				cJSON_AddItemToObject(json, "clients", arr);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			if (send(wc->fd, json_str, len + 1, 0) < 0)
				ipc_remove_watch_client(wc);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_all_tags(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_ALL_TAGS) {
			if (!json_str) {
				cJSON *json = build_all_tags_response();
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			if (send(wc->fd, json_str, len + 1, 0) < 0)
				ipc_remove_watch_client(wc);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_keymode(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_KEYMODE) {
			if (!json_str) {
				cJSON *json = cJSON_CreateObject();
				cJSON_AddStringToObject(json, "keymode", keymode.mode);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			if (send(wc->fd, json_str, len + 1, 0) < 0)
				ipc_remove_watch_client(wc);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_kb_layout(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_KB_LAYOUT) {
			if (!json_str) {
				cJSON *json = cJSON_CreateObject();
				cJSON_AddStringToObject(json, "layout", ipc_get_layout_str());
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			if (send(wc->fd, json_str, len + 1, 0) < 0)
				ipc_remove_watch_client(wc);
		}
	}
	if (json_str)
		free(json_str);
}

/* ---------- 初始化与清理 ---------- */
static int ipc_sock_fd = -1;
static struct wl_event_source *ipc_event_source = NULL;
static char ipc_socket_path[256];

void ipc_init(struct wl_event_loop *event_loop) {
	wl_list_init(&watch_clients);

	const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
	if (!xdg_runtime)
		return;

	snprintf(ipc_socket_path, sizeof(ipc_socket_path), "%s/mango-%d.sock",
			 xdg_runtime, getpid());

	ipc_sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ipc_sock_fd < 0)
		return;

	// 设置 FD_CLOEXEC
	int flags = fcntl(ipc_sock_fd, F_GETFD, 0);
	if (flags == -1 || fcntl(ipc_sock_fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
		wlr_log(WLR_ERROR, "failed to set FD_CLOEXEC on IPC socket");
		close(ipc_sock_fd);
		return;
	}
	// 设置 O_NONBLOCK
	flags = fcntl(ipc_sock_fd, F_GETFL, 0);
	if (flags == -1 || fcntl(ipc_sock_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
		wlr_log(WLR_ERROR, "failed to set O_NONBLOCK on IPC socket");
		close(ipc_sock_fd);
		return;
	}

	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	strncpy(addr.sun_path, ipc_socket_path, sizeof(addr.sun_path) - 1);

	unlink(ipc_socket_path);
	if (bind(ipc_sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(ipc_sock_fd);
		return;
	}
	listen(ipc_sock_fd, 16);

	setenv("MANGO_INSTANCE_SIGNATURE", ipc_socket_path, 1);

	ipc_event_source =
		wl_event_loop_add_fd(event_loop, ipc_sock_fd, WL_EVENT_READABLE,
							 ipc_handle_connection, event_loop);
}

void ipc_cleanup(void) {
	if (ipc_event_source)
		wl_event_source_remove(ipc_event_source);
	if (ipc_sock_fd >= 0)
		close(ipc_sock_fd);
	unlink(ipc_socket_path);
	unsetenv("MANGO_INSTANCE_SIGNATURE");

	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link)
		ipc_remove_watch_client(wc);
}
