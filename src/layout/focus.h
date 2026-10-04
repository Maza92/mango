#ifndef FOCUS_H
#define FOCUS_H

static void focus_layout(Monitor *m) {
	Client *c = NULL;
	struct wlr_box geom;

	geom.x = m->w.x;
	geom.y = m->w.y;
	geom.width = m->w.width;
	geom.height = m->w.height;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || !ISFAKETILED(c))
			continue;
		client_tile_resize(c, geom, 0);
	}
}

#endif
