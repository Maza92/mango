#ifndef FOCUS_H
#define FOCUS_H

static void focus_layout(Monitor *m) {
	Client *c = NULL;
	struct wlr_box geom;

	int32_t cur_gappov = enablegaps ? m->gappov : 0;
	int32_t cur_gappoh = enablegaps ? m->gappoh : 0;

	geom.x = m->w.x + cur_gappoh;
	geom.y = m->w.y + cur_gappov;
	geom.width = m->w.width - 2 * cur_gappoh;
	geom.height = m->w.height - 2 * cur_gappov;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || !ISFAKETILED(c))
			continue;
		client_tile_resize(c, geom, 0);
	}
}

#endif
