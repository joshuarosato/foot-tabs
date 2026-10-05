#include "tabs.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include <xkbcommon/xkbcommon-compose.h>

#define LOG_MODULE "tabs"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "config.h"
#include "debug.h"
#include "ime.h"
#include "input.h"
#include "render.h"
#include "search.h"
#include "terminal.h"
#include "url-mode.h"
#include "util.h"
#include "wayland.h"
#include "xmalloc.h"

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

    if (tll_length(win->tabs) >= TAB_MAX_COUNT) {
        LOG_INFO("refusing to open more than %d tabs", TAB_MAX_COUNT);
        term_flash(term, 50);
        render_refresh(term);
        return false;
    }

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

    /* term_init() appended the tab; move it next to the current one */
    if (term->conf->tabs.new_position == TABS_NEW_POSITION_AFTER_CURRENT) {
        /*
         * Bubble it towards the front, until it's right after the
         * current tab (swapping items; the list nodes stay put).
         */
        xassert(tll_back(win->tabs) == tab);

        tll_rforeach(win->tabs, it) {
            if (it->prev == NULL || it->prev->item == term)
                break;

            it->item = it->prev->item;
            it->prev->item = tab;
        }
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

    new->tab.activated = ++win->tab_activations;

    /* The window's size, which the new tab will be resized to */
    const float logical_width = old->scale > 0. ? old->width / old->scale : 0.;
    const float logical_height = old->scale > 0. ? old->height / old->scale : 0.;

    /* These modes use window-global surfaces */
    if (old->tab.rename.active)
        tab_rename_cancel(old);
    render_flush_pending_resize(old);
    if (old->is_searching)
        search_cancel(old);
    if (urls_mode_is_active(old))
        urls_reset(old);

    win->term = new;

    /* Font size changes in one tab apply to all tabs. Must be done
     * before resizing, since it changes the cell size */
    if (new->conf->tabs.shared_font_size && !new->shutdown.in_progress)
        term_font_size_copy(new, old);

    /* A new tab doesn't have a grid until it has been sized; must be
     * done before giving it keyboard focus */
    if (!new->shutdown.in_progress && logical_width > 0. && logical_height > 0.)
        wayl_win_tab_sync_size(new, logical_width, logical_height, true);

    switch_seat_focus(old, new);

    /* E.g. the window is being closed, and all tabs are shutting down */
    if (new->shutdown.in_progress)
        return;

    /* The window surface holds the previous tab's content */
    render_refresh_full(new);
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

struct terminal *
tab_at_index(const struct wl_window *win, size_t idx)
{
    size_t i = 0;
    tll_foreach(win->tabs, it) {
        if (i++ == idx)
            return it->item;
    }
    return NULL;
}

void
tab_activate_index(struct wl_window *win, size_t idx)
{
    struct terminal *tab = tab_at_index(win, idx);
    if (tab != NULL)
        tab_activate(tab);
}

void
tab_activate_last(struct wl_window *win)
{
    tab_activate_index(win, tll_length(win->tabs) - 1);
}

/* The most recently active tab, other than the active one, that isn't
 * being closed */
static struct terminal *
last_used_tab(const struct wl_window *win)
{
    struct terminal *last = NULL;

    tll_foreach(win->tabs, it) {
        struct terminal *tab = it->item;

        if (tab == win->term || tab->shutdown.in_progress)
            continue;

        if (last == NULL || tab->tab.activated > last->tab.activated)
            last = tab;
    }

    return last;
}

void
tab_activate_last_used(struct wl_window *win)
{
    struct terminal *tab = last_used_tab(win);
    if (tab != NULL)
        tab_activate(tab);
}

void
tab_move(struct wl_window *win, int direction)
{
    tll_foreach(win->tabs, it) {
        if (it->item != win->term)
            continue;

        /* Doesn't wrap around */
        __typeof__(it) other = direction < 0 ? it->prev : it->next;
        if (other == NULL)
            return;

        it->item = other->item;
        other->item = win->term;
        render_refresh_tab_bar(win);
        return;
    }
}

/* Moves the active tab to position 'idx' */
void
tab_move_to(struct wl_window *win, size_t idx)
{
    const size_t count = tll_length(win->tabs);
    const size_t target = min(idx, count - 1);

    size_t current = 0;
    tll_foreach(win->tabs, it) {
        if (it->item == win->term)
            break;
        current++;
    }

    for (; current < target; current++)
        tab_move(win, 1);
    for (; current > target; current--)
        tab_move(win, -1);
}

void
tab_move_last(struct wl_window *win)
{
    tab_move_to(win, tll_length(win->tabs) - 1);
}

/*
 * Moves the active tab to the tab bar position under 'x' (which may
 * be outside the tab bar, while dragging).
 */
void
tab_drag(struct wl_window *win, int x)
{
    const int count = tll_length(win->tabs);
    const int width = win->term->width;

    if (count < 2 || width <= 0)
        return;

    tab_move_to(win, max(0, min(count - 1, (int)((int64_t)x * count / width))));
}

const char *
tab_title(const struct terminal *term)
{
    if (term->tab.title != NULL)
        return term->tab.title;
    return term->window_title != NULL ? term->window_title : "foot";
}

static void
rename_append(struct terminal *term, char32_t wc)
{
    static const size_t max_len = 256;
    __typeof__(term->tab.rename) *r = &term->tab.rename;

    if (r->len >= max_len)
        return;

    if (r->len + 1 >= r->sz) {
        r->sz = r->sz == 0 ? 64 : r->sz * 2;
        r->buf = xrealloc(r->buf, r->sz * sizeof(r->buf[0]));
    }

    r->buf[r->len++] = wc;
    r->buf[r->len] = U'\0';
}

static void
rename_end(struct terminal *term)
{
    __typeof__(term->tab.rename) *r = &term->tab.rename;

    free(r->buf);
    r->buf = NULL;
    r->len = r->sz = 0;
    r->active = false;

    /* A single tab's bar was only shown while renaming */
    render_refresh_tab_bar(term->window);
}

void
tab_rename_start(struct terminal *term)
{
    __typeof__(term->tab.rename) *r = &term->tab.rename;

    if (r->active || term->shutdown.in_progress)
        return;

    if (term->is_searching)
        search_cancel(term);
    urls_reset(term);

    r->active = true;

    /* Start with the current label, so that it can be edited */
    const char *title = term->tab.title != NULL
        ? term->tab.title : term->window_title;
    char32_t *wcs = ambstoc32(title);

    if (wcs != NULL) {
        for (const char32_t *wc = wcs; *wc != U'\0'; wc++)
            rename_append(term, *wc);
        free(wcs);
    }

    render_refresh_tab_bar(term->window);
}

void
tab_rename_commit(struct terminal *term)
{
    __typeof__(term->tab.rename) *r = &term->tab.rename;
    xassert(r->active);

    free(term->tab.title);
    term->tab.title = r->len > 0 ? ac32tombs(r->buf) : NULL;
    rename_end(term);
}

void
tab_rename_cancel(struct terminal *term)
{
    xassert(term->tab.rename.active);
    rename_end(term);
}

void
tab_rename_input(struct seat *seat, struct terminal *term, uint32_t key,
                 xkb_keysym_t sym)
{
    __typeof__(term->tab.rename) *r = &term->tab.rename;
    xassert(r->active);

    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        tab_rename_commit(term);
        return;

    case XKB_KEY_Escape:
        tab_rename_cancel(term);
        return;

    case XKB_KEY_BackSpace:
        if (seat->kbd.ctrl) {
            /* Delete previous word */
            while (r->len > 0 && r->buf[r->len - 1] == U' ')
                r->len--;
            while (r->len > 0 && r->buf[r->len - 1] != U' ')
                r->len--;
        } else if (r->len > 0)
            r->len--;

        if (r->buf != NULL)
            r->buf[r->len] = U'\0';
        render_refresh_tab_bar(term->window);
        return;
    }

    if (seat->kbd.ctrl) {
        switch (sym) {
        case XKB_KEY_u:
            /* Clear */
            r->len = 0;
            if (r->buf != NULL)
                r->buf[0] = U'\0';
            render_refresh_tab_bar(term->window);
            break;

        case XKB_KEY_c:
        case XKB_KEY_g:
            tab_rename_cancel(term);
            break;
        }

        /* Ignore all other control sequences */
        return;
    }

    struct xkb_compose_state *compose = seat->kbd.xkb_compose_state;
    const enum xkb_compose_status compose_status = compose != NULL
        ? xkb_compose_state_get_status(compose)
        : XKB_COMPOSE_NOTHING;

    char buf[64] = {0};
    int count = 0;

    if (compose_status == XKB_COMPOSE_COMPOSED) {
        count = xkb_compose_state_get_utf8(compose, buf, sizeof(buf));
        xkb_compose_state_reset(compose);
    } else if (compose_status == XKB_COMPOSE_NOTHING)
        count = xkb_state_key_get_utf8(seat->kbd.xkb_state, key, buf, sizeof(buf));

    if (count <= 0 || count >= (int)sizeof(buf))
        return;

    char32_t *wcs = ambstoc32(buf);
    if (wcs == NULL)
        return;

    for (const char32_t *wc = wcs; *wc != U'\0'; wc++) {
        /* Skip control characters */
        if (*wc < 0x20 || (*wc >= 0x7f && *wc < 0xa0))
            continue;
        rename_append(term, *wc);
    }

    free(wcs);
    render_refresh_tab_bar(term->window);
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

static bool
in_close_scope(enum tab_close_scope scope, bool is_active, bool after_active)
{
    switch (scope) {
    case TAB_CLOSE_NONE:   return false;
    case TAB_CLOSE_ALL:    return true;
    case TAB_CLOSE_LEFT:   return !is_active && !after_active;
    case TAB_CLOSE_RIGHT:  return after_active;
    case TAB_CLOSE_OTHERS: return !is_active;
    }

    BUG("unhandled tab close scope: %d", scope);
    return false;
}

size_t
tab_close_count(const struct wl_window *win, enum tab_close_scope scope)
{
    size_t count = 0;
    bool after_active = false;

    tll_foreach(win->tabs, it) {
        const struct terminal *tab = it->item;
        const bool is_active = tab == win->term;

        if (!tab->shutdown.in_progress &&
            in_close_scope(scope, is_active, after_active))
        {
            count++;
        }

        if (is_active)
            after_active = true;
    }

    return count;
}

void
tab_close(struct wl_window *win, enum tab_close_scope scope)
{
    bool after_active = false;

    /* term_shutdown() is asynchronous; the tabs are detached later */
    tll_foreach(win->tabs, it) {
        struct terminal *tab = it->item;
        const bool is_active = tab == win->term;

        if (in_close_scope(scope, is_active, after_active))
            term_shutdown(tab);

        if (is_active)
            after_active = true;
    }
}

void
tab_request_close(struct wl_window *win, enum tab_close_scope scope)
{
    struct terminal *term = win->term;
    xassert(scope != TAB_CLOSE_NONE);

    const bool confirm = scope == TAB_CLOSE_ALL
        ? term->conf->tabs.confirm_close
        : term->conf->tabs.confirm_close_multiple;

    /* Requesting the same close while asking, closes immediately */
    if (!confirm ||
        win->confirm_close == scope ||
        tab_close_count(win, scope) < 2)
    {
        tab_close(win, scope);
        return;
    }

    /* All keyboard input goes to the prompt; leave other input modes */
    if (term->tab.rename.active)
        tab_rename_cancel(term);
    if (term->is_searching)
        search_cancel(term);
    urls_reset(term);

    win->confirm_close = scope;
    render_refresh(term);
}

void
tab_request_close_window(struct wl_window *win)
{
    tab_request_close(win, TAB_CLOSE_ALL);
}

static void
confirm_close_dismiss(struct wl_window *win)
{
    win->confirm_close = TAB_CLOSE_NONE;
    render_refresh(win->term);
}

void
tab_confirm_close_input(struct terminal *term, xkb_keysym_t sym)
{
    struct wl_window *win = term->window;
    const enum tab_close_scope scope = win->confirm_close;
    xassert(scope != TAB_CLOSE_NONE);

    switch (sym) {
    case XKB_KEY_y:
    case XKB_KEY_Y:
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        /* Unless closing the window, the active tab stays open */
        if (scope != TAB_CLOSE_ALL)
            confirm_close_dismiss(win);
        tab_close(win, scope);
        break;

    case XKB_KEY_n:
    case XKB_KEY_N:
    case XKB_KEY_Escape:
        confirm_close_dismiss(win);
        break;
    }
}

bool
tab_detach(struct terminal *term)
{
    struct wl_window *win = term->window;
    const bool tab_bar_was_visible = tab_bar_visible(win);

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
        struct terminal *last_used =
            term->conf->tabs.activate_on_close == TABS_ACTIVATE_ON_CLOSE_LAST_USED
                ? last_used_tab(win) : NULL;

        if (last_used != NULL)
            tab_activate(last_used);
        else {
            /* Like most tabbed applications, prefer the tab to the right */
            tab_activate_index(win, min(idx, tll_length(win->tabs) - 1));
        }
    } else if (tab_bar_was_visible && !tab_bar_visible(win)) {
        /* The tab bar was hidden; give its space to the grid */
        const struct terminal *active = win->term;
        wayl_win_tab_sync_size(
            win->term, active->width / active->scale,
            active->height / active->scale, true);
    }

    xassert(win->term != term);

    if (win->confirm_close == TAB_CLOSE_ALL) {
        if (tll_length(win->tabs) < 2)
            confirm_close_dismiss(win);
    } else if (win->confirm_close != TAB_CLOSE_NONE) {
        /* Tabs closed by themselves; update, or drop, the question */
        if (tab_close_count(win, win->confirm_close) < 2)
            confirm_close_dismiss(win);
        else
            render_refresh(win->term);
    }

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
    switch (win->term->conf->tabs.show_bar) {
    case TABS_SHOW_BAR_AUTO:   return tll_length(win->tabs) > 1;
    case TABS_SHOW_BAR_ALWAYS: return true;
    case TABS_SHOW_BAR_NEVER:  return false;
    }

    BUG("unhandled show-bar value: %d", win->term->conf->tabs.show_bar);
    return false;
}

bool
tab_bar_shown(const struct wl_window *win)
{
    return tab_bar_visible(win) || win->term->tab.rename.active;
}

int
tab_bar_strip_height(const struct terminal *term)
{
    return roundf(term->conf->csd.title_height * term->scale);
}

int
tab_bar_height(const struct terminal *term)
{
    return tab_bar_visible(term->window) ? tab_bar_strip_height(term) : 0;
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
