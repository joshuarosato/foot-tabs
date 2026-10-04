#include "tabs.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define LOG_MODULE "tabs"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "debug.h"
#include "ime.h"
#include "input.h"
#include "render.h"
#include "search.h"
#include "shm.h"
#include "terminal.h"
#include "url-mode.h"
#include "util.h"
#include "wayland.h"

static void
tab_shutdown_cb(void *data, int exit_code)
{
    /* Tabs own a private copy of the configuration */
    struct config *conf = data;
    config_free(conf);
    free(conf);
}

bool
tab_new(struct terminal *term)
{
    struct wl_window *win = term->window;

    if (!win->is_configured)
        return false;

    /*
     * The configuration may be owned by another terminal instance
     * (e.g. a footclient instance with command line overrides), that
     * may exit before this tab does.
     */
    struct config *conf = config_clone(term->conf);

    struct terminal *tab = term_init(
        conf, term->fdm, term->reaper, term->wl, term->foot_exe, term->cwd,
        NULL, NULL, 0, NULL, NULL, win, &tab_shutdown_cb, conf);

    if (tab == NULL) {
        LOG_ERR("failed to instantiate new tab");
        config_free(conf);
        free(conf);
        return false;
    }

    /* Size the grid before we start reading from the PTY */
    tab_activate(tab);
    term_window_configured(tab);
    return true;
}

static void
switch_seat_focus(struct terminal *old, struct terminal *new)
{
    bool had_kbd_focus = false;

    new->active_surface = old->active_surface;
    old->active_surface = TERM_SURF_NONE;

    tll_foreach(new->wl->seats, it) {
        struct seat *seat = &it->item;

        if (seat->kbd_focus == old) {
            ime_reset_preedit(seat);
            seat->kbd_focus = new;
            had_kbd_focus = true;
        }

        if (seat->mouse_focus == old) {
            seat->mouse_focus = new;

            /* The new tab's grid geometry may differ */
            if (new->active_surface == TERM_SURF_GRID && !new->shutdown.in_progress)
                mouse_coord_pixel_to_cell(seat, new, seat->mouse.x, seat->mouse.y);
        }

        if (seat->ime_focus == old) {
            ime_disable(seat);
            seat->ime_focus = new;
            ime_enable(seat);
        }
    }

    if (old->visual_focus)
        term_visual_focus_in(new);
    else
        term_visual_focus_out(new);

    if (had_kbd_focus)
        term_kbd_focus_in(new);

    /* A tab being closed has already closed its PTY */
    if (!old->shutdown.in_progress) {
        if (had_kbd_focus)
            term_kbd_focus_out(old);
        term_visual_focus_out(old);
    }
}

void
tab_activate(struct terminal *new)
{
    struct wl_window *win = new->window;
    struct terminal *old = win->term;

    if (new == old)
        return;

    LOG_DBG("activating tab %p (previous: %p)", (void *)new, (void *)old);

    /* The window's size, which the new tab will be resized to */
    const float logical_width = old->scale > 0. ? old->width / old->scale : 0.;
    const float logical_height = old->scale > 0. ? old->height / old->scale : 0.;

    /* These modes use window-global surfaces */
    render_flush_pending_resize(old);
    if (old->is_searching)
        search_cancel(old);
    if (urls_mode_is_active(old))
        urls_reset(old);

    win->term = new;

    /* A new tab doesn't have a grid until it has been sized; must be
     * done before giving it keyboard focus */
    if (!new->shutdown.in_progress && logical_width > 0. && logical_height > 0.)
        wayl_win_tab_sync_size(new, logical_width, logical_height, true);

    switch_seat_focus(old, new);

    /* E.g. the window is being closed, and all tabs are shutting down */
    if (new->shutdown.in_progress)
        return;

    /* The window surface holds the previous tab's content; force a
     * full repaint */
    render_wait_for_preapply_damage(new);
    shm_unref(new->render.last_buf);
    new->render.last_buf = NULL;
    term_damage_view(new);
    render_refresh(new);
    render_refresh_csd(new);
    render_refresh_title(new);
    render_refresh_app_id(new);
    render_refresh_icon(new);
    render_refresh_tab_bar(win);
    wayl_win_alpha_changed(win);

    tll_foreach(new->wl->seats, it) {
        if (it->item.mouse_focus == new)
            term_xcursor_update_for_seat(new, &it->item);
    }
}

void
tab_activate_index(struct wl_window *win, size_t idx)
{
    size_t i = 0;
    tll_foreach(win->tabs, it) {
        if (i++ == idx) {
            tab_activate(it->item);
            return;
        }
    }
}

void
tab_cycle(struct wl_window *win, int direction)
{
    const size_t count = tll_length(win->tabs);
    if (count < 2)
        return;

    size_t idx = 0;
    tll_foreach(win->tabs, it) {
        if (it->item == win->term)
            break;
        idx++;
    }

    xassert(idx < count);

    long next = ((long)idx + direction) % (long)count;
    if (next < 0)
        next += count;
    tab_activate_index(win, next);
}

void
tab_close_all(struct wl_window *win)
{
    /* term_shutdown() is asynchronous; the tabs are detached later */
    tll_foreach(win->tabs, it)
        term_shutdown(it->item);
}

bool
tab_detach(struct terminal *term)
{
    struct wl_window *win = term->window;

    size_t idx = 0;
    bool found = false;
    tll_foreach(win->tabs, it) {
        if (it->item == term) {
            tll_remove(win->tabs, it);
            found = true;
            break;
        }
        idx++;
    }

    xassert(found);
    (void)found;

    if (tll_length(win->tabs) == 0) {
        xassert(win->term == term);
        return true;
    }

    if (win->term == term) {
        /* Like most tabbed applications, prefer the tab to the right */
        tab_activate_index(win, min(idx, tll_length(win->tabs) - 1));
    } else if (!tab_bar_visible(win)) {
        /* The tab bar was hidden; give its space to the grid */
        const struct terminal *active = win->term;
        wayl_win_tab_sync_size(
            win->term, active->width / active->scale,
            active->height / active->scale, true);
    }

    xassert(win->term != term);

    tll_foreach(term->wl->seats, it) {
        xassert(it->item.kbd_focus != term);
        xassert(it->item.mouse_focus != term);
        xassert(it->item.ime_focus != term);
    }

    render_refresh_tab_bar(win);
    return false;
}

bool
tab_bar_visible(const struct wl_window *win)
{
    return tll_length(win->tabs) > 1;
}

int
tab_bar_height(const struct terminal *term)
{
    if (!tab_bar_visible(term->window))
        return 0;
    return roundf(term->conf->csd.title_height * term->scale);
}

void
tab_bar_tab_extent(const struct wl_window *win, int width, size_t idx,
                   int *x0, int *x1)
{
    /* All tabs have the same width, and together they fill the bar */
    const size_t count = tll_length(win->tabs);
    xassert(count > 0);

    *x0 = (int64_t)width * idx / count;
    *x1 = (int64_t)width * (idx + 1) / count;
}

int
tab_bar_tab_at(const struct wl_window *win, int x)
{
    const int width = win->term->width;
    const size_t count = tll_length(win->tabs);

    if (x < 0 || x >= width || count == 0)
        return -1;

    return (int64_t)x * count / width;
}
