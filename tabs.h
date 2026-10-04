#pragma once

#include <stdbool.h>
#include <stddef.h>

struct terminal;
struct wl_window;

/*
 * Tabs: a window holds one or more terminals (tabs). Only the active
 * tab (win->term) renders, and receives input. Background tabs keep
 * reading from their PTYs, and are resized and repainted when they
 * are activated.
 */

bool tab_new(struct terminal *term);
void tab_activate(struct terminal *term);
void tab_activate_index(struct wl_window *win, size_t idx);
void tab_cycle(struct wl_window *win, int direction);
void tab_close_all(struct wl_window *win);

/*
 * Removes the terminal from its window's tab list. Returns true if
 * it was the last tab, in which case the caller should destroy the
 * window.
 */
bool tab_detach(struct terminal *term);

bool tab_bar_visible(const struct wl_window *win);
int tab_bar_height(const struct terminal *term);
void tab_bar_tab_extent(
    const struct wl_window *win, int width, size_t idx, int *x0, int *x1);
int tab_bar_tab_at(const struct wl_window *win, int x);
