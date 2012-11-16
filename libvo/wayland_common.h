/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPLAYER_WAYLAND_COMMON_H
#define MPLAYER_WAYLAND_COMMON_H

#include <stdint.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include "config.h"

    /* The button values are codes from linux/input.h but we can't
     * include it because of conflicting definitions in mplayer. */
#define BTN_LEFT 0x110

#define MOD_SHIFT_MASK		0x01
#define MOD_ALT_MASK		0x02
#define MOD_CONTROL_MASK	0x04

enum vo_wayland_window_type {
    TYPE_TOPLEVEL,
    TYPE_FULLSCREEN
};

struct vo;

struct vo_wayland_display {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shell *shell;
    struct wl_output *output;

    struct {
        struct wl_shm *shm;
        struct wl_cursor *default_cursor;
        struct wl_cursor_theme *theme;
        struct wl_surface *surface;

        /* save timer and pointer for fading out */
        struct wl_pointer *pointer;
        uint32_t serial;
        int timer_fd;
    } cursor;

    int mode_received;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t output_x;
    uint32_t output_y;

    uint32_t formats;
    uint32_t mask;
};

struct vo_wayland_window {
    uint32_t width;
    uint32_t height;
    uint32_t p_width;
    uint32_t p_height;

    uint32_t pending_width;
    uint32_t pending_height;
    uint32_t edges;
    int resize_needed;

    struct wl_surface *surface;
    struct wl_shell_surface *shell_surface;
    struct wl_buffer *buffer;
    struct wl_callback *callback;

    int events; /* mplayer events */

    enum vo_wayland_window_type type; /* is fullscreen */
};

struct vo_wayland_input {
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;

    struct {
        struct xkb_context *context;
        struct xkb_keymap *keymap;
        struct xkb_state *state;
        xkb_mod_mask_t shift_mask;
        xkb_mod_mask_t control_mask;
        xkb_mod_mask_t alt_mask;
    } xkb;

    int modifiers;
    int events;

    struct {
        uint32_t sym;
        uint32_t key;
        uint32_t time;
        int timer_fd;
    } repeat;
};

struct vo_wayland_state {
    struct vo *vo;

    struct vo_wayland_display *display;
    struct vo_wayland_window *window;
    struct vo_wayland_input *input;
};

int vo_wayland_init(struct vo *vo);
void vo_wayland_uninit(struct vo *vo);
void vo_wayland_ontop(struct vo *vo);
void vo_wayland_border(struct vo *vo);
void vo_wayland_fullscreen(struct vo *vo);
void vo_wayland_update_xinerama_info(struct vo *vo);
int vo_wayland_check_events(struct vo *vo);

#endif /* MPLAYER_WAYLAND_COMMON_H */

