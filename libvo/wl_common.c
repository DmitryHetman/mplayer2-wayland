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

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>
#include <limits.h>
#include <assert.h>

#include <unistd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>

#include "config.h"
#include "bstr.h"
#include "options.h"
#include "mp_msg.h"
#include "mp_fifo.h"
#include "libavutil/common.h"
#include "talloc.h"

#include "wl_common.h"

#include "video_out.h"
#include "aspect.h"
#include "geometry.h"
#include "osdep/timer.h"

#include "subopt-helper.h"

#include "input/input.h"
#include "input/keycodes.h"

static void create_display (struct wl_priv *wl);
static void create_window (struct wl_priv *wl, int width, int height);

/* SHELL SURFACE LISTENER  */
static void ssurface_handle_ping (void *data,
        struct wl_shell_surface *shell_surface, uint32_t serial)
{
    wl_shell_surface_pong(shell_surface, serial);
}

static void ssurface_handle_configure (void *data,
        struct wl_shell_surface *shell_surface,
        uint32_t edges, int32_t width, int32_t height)
{
}

static void ssurface_handle_popup_done (void *data,
        struct wl_shell_surface *shell_surface)
{
}

const struct wl_shell_surface_listener shell_surface_listener = {
    ssurface_handle_ping,
    ssurface_handle_configure,
    ssurface_handle_popup_done
};

/* OUTPUT LISTENER */
static void output_handle_geometry (void *data, struct wl_output *wl_output,
        int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
        int32_t subpixel, const char *make, const char *model,
        int32_t transform)
{
}

static void output_handle_mode (void *data, struct wl_output *wl_output,
        uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
    struct vo_wl_display *d = data;
    if ((flags & WL_OUTPUT_MODE_PREFERRED) == WL_OUTPUT_MODE_PREFERRED) {
        d->output_height = height;
        d->output_width = width;
        d->mode_received = 1;
    }
}

const struct wl_output_listener output_listener = {
    output_handle_geometry,
    output_handle_mode
};

/* KEY LOOKUP */
static const struct mp_keymap keymap[] = {
    // special keys
    {XKB_KEY_Pause, KEY_PAUSE}, {XKB_KEY_Escape, KEY_ESC},
    {XKB_KEY_BackSpace, KEY_BS}, {XKB_KEY_Tab, KEY_TAB},
    {XKB_KEY_Return, KEY_ENTER}, {XKB_KEY_Menu, KEY_MENU},
    {XKB_KEY_Print, KEY_PRINT},

    // cursor keys
    {XKB_KEY_Left, KEY_LEFT}, {XKB_KEY_Right, KEY_RIGHT},
    {XKB_KEY_Up, KEY_UP}, {XKB_KEY_Down, KEY_DOWN},

    // navigation block
    {XKB_KEY_Insert, KEY_INSERT}, {XKB_KEY_Delete, KEY_DELETE},
    {XKB_KEY_Home, KEY_HOME}, {XKB_KEY_End, KEY_END},
    {XKB_KEY_Page_Up, KEY_PAGE_UP}, {XKB_KEY_Page_Down, KEY_PAGE_DOWN},

    // F-keys
    {XKB_KEY_F1, KEY_F+1}, {XKB_KEY_F2, KEY_F+2}, {XKB_KEY_F3, KEY_F+3},
    {XKB_KEY_F4, KEY_F+4}, {XKB_KEY_F5, KEY_F+5}, {XKB_KEY_F6, KEY_F+6},
    {XKB_KEY_F7, KEY_F+7}, {XKB_KEY_F8, KEY_F+8}, {XKB_KEY_F9, KEY_F+9},
    {XKB_KEY_F10, KEY_F+10}, {XKB_KEY_F11, KEY_F+11}, {XKB_KEY_F12, KEY_F+12},

    // numpad independent of numlock
    {XKB_KEY_KP_Subtract, '-'}, {XKB_KEY_KP_Add, '+'},
    {XKB_KEY_KP_Multiply, '*'}, {XKB_KEY_KP_Divide, '/'},
    {XKB_KEY_KP_Enter, KEY_KPENTER},

    // numpad with numlock
    {XKB_KEY_KP_0, KEY_KP0}, {XKB_KEY_KP_1, KEY_KP1}, {XKB_KEY_KP_2, KEY_KP2},
    {XKB_KEY_KP_3, KEY_KP3}, {XKB_KEY_KP_4, KEY_KP4}, {XKB_KEY_KP_5, KEY_KP5},
    {XKB_KEY_KP_6, KEY_KP6}, {XKB_KEY_KP_7, KEY_KP7}, {XKB_KEY_KP_8, KEY_KP8},
    {XKB_KEY_KP_9, KEY_KP9}, {XKB_KEY_KP_Decimal, KEY_KPDEC},
    {XKB_KEY_KP_Separator, KEY_KPDEC},

    // numpad without numlock
    {XKB_KEY_KP_Insert, KEY_KPINS}, {XKB_KEY_KP_End, KEY_KP1},
    {XKB_KEY_KP_Down, KEY_KP2}, {XKB_KEY_KP_Page_Down, KEY_KP3},
    {XKB_KEY_KP_Left, KEY_KP4}, {XKB_KEY_KP_Begin, KEY_KP5},
    {XKB_KEY_KP_Right, KEY_KP6}, {XKB_KEY_KP_Home, KEY_KP7},
    {XKB_KEY_KP_Up, KEY_KP8}, {XKB_KEY_KP_Page_Up, KEY_KP9},
    {XKB_KEY_KP_Delete, KEY_KPDEL},

    {0, 0}
};

static int vo_wl_lookupkey(int key)
{
    static const char *passthrough_keys
        = " -+*/<>`~!@#$%^&()_{}:;\"\',.?\\|=[]";

    int mpkey = 0;
    if ((key >= 'a' && key <= 'z') ||
        (key >= 'A' && key <= 'Z') ||
        (key >= '0' && key <= '9') ||
        (key >  0   && key <  256 && strchr(passthrough_keys, key)))
        mpkey = key;

    if (!mpkey)
        mpkey = lookup_keymap_table(keymap, key);

    return mpkey;
}

/* KEYBOARD LISTENER */
static void keyboard_handle_keymap(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t format, int32_t fd, uint32_t size)
{
    struct vo_wl_input *input = ((struct wl_priv *) data)->input;
    char *map_str;

    if(!data) {
        close(fd);
        return;
    }

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map_str == MAP_FAILED) {
        close(fd);
        return;
    }

    input->xkb.keymap = xkb_map_new_from_string(input->xkb.context,
            map_str, XKB_KEYMAP_FORMAT_TEXT_V1, 0);

    munmap(map_str, size);
    close(fd);

    if (!input->xkb.keymap) {
        mp_msg(MSGT_VO, MSGL_ERR, "[wl] failed to compile keymap.\n");
        return;
    }

    input->xkb.state = xkb_state_new(input->xkb.keymap);
    if (!input->xkb.state) {
        mp_msg(MSGT_VO, MSGL_ERR, "[wl] failed to create XKB state.\n");
        xkb_map_unref(input->xkb.keymap);
        input->xkb.keymap = NULL;
        return;
    }

	input->xkb.control_mask =
		1 << xkb_map_mod_get_index(input->xkb.keymap, "Control");
	input->xkb.alt_mask =
		1 << xkb_map_mod_get_index(input->xkb.keymap, "Mod1");
    input->xkb.shift_mask =
        1 << xkb_map_mod_get_index(input->xkb.keymap, "Shift");
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, struct wl_surface *surface, struct wl_array *keys)
{
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, struct wl_surface *surface)
{
}

static void keyboard_handle_key(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    struct wl_priv *wl = data;
    struct vo_wl_input *input = wl->input;
    uint32_t code, num_syms;

    struct itimerspec its;

    const xkb_keysym_t *syms;
    xkb_keysym_t sym;
    xkb_mod_mask_t mask;

    code = key + 8;
    num_syms = xkb_key_get_syms(input->xkb.state, code, &syms);

    mask = xkb_state_serialize_mods(input->xkb.state,
            XKB_STATE_DEPRESSED | XKB_STATE_LATCHED);

    input->modifiers = 0;
    if (mask & input->xkb.control_mask)
        input->modifiers |= MOD_CONTROL_MASK;
    if (mask & input->xkb.alt_mask)
        input->modifiers |= MOD_ALT_MASK;
    if (mask & input->xkb.shift_mask)
        input->modifiers |= MOD_SHIFT_MASK;

    sym = XKB_KEY_NoSymbol;
    if (num_syms == 1)
        sym = syms[0];

    if (sym != XKB_KEY_NoSymbol && state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        int mpkey = vo_wl_lookupkey(sym);
        if (mpkey)
	        mplayer_put_key(wl->vo->key_fifo, mpkey);
        input->events |= VO_EVENT_KEYPRESS;
    }

    if (state == WL_KEYBOARD_KEY_STATE_RELEASED && key == input->repeat.key) {
        its.it_interval.tv_sec = 0;
        its.it_interval.tv_nsec = 0;
        its.it_value.tv_sec = 0;
        its.it_value.tv_nsec = 0;
        timerfd_settime(input->repeat.timer_fd, 0, &its, NULL);
    }
    else if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        input->repeat.sym = sym;
        input->repeat.key = key;
        input->repeat.time = time;
        its.it_interval.tv_sec = 0;
        its.it_interval.tv_nsec = 25 * 1000 * 1000;
        its.it_value.tv_sec = 0;
        its.it_value.tv_nsec = 400 * 1000 * 1000;
        timerfd_settime(input->repeat.timer_fd, 0, &its, NULL);
    }
}

static void keyboard_handle_modifiers(void *data,
        struct wl_keyboard *wl_keyboard, uint32_t serial,
        uint32_t mods_depressed, uint32_t mods_latched,
        uint32_t mods_locked, uint32_t group)
{
    struct vo_wl_input *input = ((struct wl_priv *) data)->input;

    xkb_state_update_mask(input->xkb.state, mods_depressed, mods_latched,
            mods_locked, 0, 0, group);
}

const struct wl_keyboard_listener keyboard_listener = {
    keyboard_handle_keymap,
    keyboard_handle_enter,
    keyboard_handle_leave,
    keyboard_handle_key,
    keyboard_handle_modifiers
};

/* POINTER LISTENER */
static void pointer_handle_enter(void *data, struct wl_pointer *pointer,
        uint32_t serial, struct wl_surface *surface,
        wl_fixed_t sx_w, wl_fixed_t sy_w)
{
    struct wl_priv *wl = data;
    struct vo_wl_display * display = wl->display;

    struct wl_buffer *buffer;
    struct wl_cursor_image *image;

    if (wl->window->type == TYPE_FULLSCREEN)
        wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
    else if (display->cursor.default_cursor) {
        image = display->cursor.default_cursor->images[0];
        buffer = wl_cursor_image_get_buffer(image);
        wl_pointer_set_cursor(pointer, serial, display->cursor.surface,
                image->hotspot_x, image->hotspot_y);
        wl_surface_attach(display->cursor.surface, buffer, 0, 0);
        wl_surface_damage(display->cursor.surface, 0, 0,
                image->width, image->height);
        wl_surface_commit(display->cursor.surface);
    }

}

static void pointer_handle_leave(void *data, struct wl_pointer *pointer,
        uint32_t serial, struct wl_surface *surface)
{
}

static void pointer_handle_motion(void *data, struct wl_pointer *pointer,
        uint32_t time, wl_fixed_t sx_w, wl_fixed_t sy_w)
{
}

static void pointer_handle_button(void *data, struct wl_pointer *pointer,
        uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    struct wl_priv *wl = data;

    mplayer_put_key(wl->vo->key_fifo, MOUSE_BTN0 + (button - BTN_LEFT) |
        ((state == WL_POINTER_BUTTON_STATE_PRESSED) ? MP_KEY_DOWN : 0));
}

static void pointer_handle_axis(void *data, struct wl_pointer *pointer,
        uint32_t time, uint32_t axis, wl_fixed_t value)
{
    struct wl_priv *wl = data;

    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        if (value > 0)
            mplayer_put_key(wl->vo->key_fifo, MOUSE_BTN4);
        if (value < 0)
            mplayer_put_key(wl->vo->key_fifo, MOUSE_BTN3);
    }
}

static const struct wl_pointer_listener pointer_listener = {
	pointer_handle_enter,
	pointer_handle_leave,
	pointer_handle_motion,
	pointer_handle_button,
	pointer_handle_axis,
};

static void seat_handle_capabilities(void *data, struct wl_seat *seat,
        enum wl_seat_capability caps)
{
    struct wl_priv *wl = data;

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl->input->keyboard) {
        wl->input->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_set_user_data(wl->input->keyboard, wl);
        wl_keyboard_add_listener(wl->input->keyboard, &keyboard_listener, wl);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl->input->pointer) {
        wl->input->pointer = wl_seat_get_pointer(seat);
        wl_pointer_set_user_data(wl->input->pointer, wl);
        wl_pointer_add_listener(wl->input->pointer, &pointer_listener, wl);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && wl->input->keyboard) {
        wl_keyboard_destroy(wl->input->keyboard);
        wl->input->keyboard = NULL;
    }
}

static const struct wl_seat_listener seat_listener = {
    seat_handle_capabilities,
};

/* SHM LISTENER */
static void shm_handle_format(void *data, struct wl_shm *wl_shm,
        uint32_t format)
{
    struct vo_wl_display *d = data;
    d->formats |= (1 << format);
}

const struct wl_shm_listener shm_listener = {
    shm_handle_format
};

static void registry_handle_global (void *data, struct wl_registry *registry,
        uint32_t id, const char *interface, uint32_t version)
{
    struct wl_priv *wl = data;
    struct vo_wl_display *d = wl->display;
    if (strcmp(interface, "wl_compositor") == 0) {
        d->compositor = wl_registry_bind(d->registry, id,
                &wl_compositor_interface, 1);
    }
    else if (strcmp(interface, "wl_shell") == 0) {
        d->shell = wl_registry_bind(d->registry, id, &wl_shell_interface, 1);
    }
    else if (strcmp(interface, "wl_shm") == 0) {
        d->cursor.shm = wl_registry_bind(d->registry, id, &wl_shm_interface, 1);
        d->cursor.theme = wl_cursor_theme_load(NULL, 32, d->cursor.shm);
        d->cursor.default_cursor =
            wl_cursor_theme_get_cursor(d->cursor.theme, "left_ptr");
        wl_shm_add_listener(d->cursor.shm, &shm_listener, d);
    }
    else if (strcmp(interface, "wl_output") == 0) {
        d->output = wl_registry_bind(d->registry, id, &wl_output_interface, 1);
        wl_output_add_listener(d->output, &output_listener, d);
    }
    else if (strcmp(interface, "wl_seat") == 0) {
        wl->input->seat = wl_registry_bind(d->registry, id,
                &wl_seat_interface, 1);
        wl_seat_add_listener(wl->input->seat, &seat_listener, wl);
    }
}

static void registry_handle_global_remove (void *data,
        struct wl_registry *registry, uint32_t id)
{
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};

static void create_display (struct wl_priv *wl)
{
    if (wl->display)
        return;

    wl->display = malloc(sizeof(struct vo_wl_display));
    wl->display->display = wl_display_connect(NULL);

    assert(wl->display->display);
    wl->display->registry = wl_display_get_registry(wl->display->display);
    wl_registry_add_listener(wl->display->registry, &registry_listener,
            wl);

    wl->display->mode_received = 0;
    wl->display->formats = 0;
    wl->display->mask = 0;
    wl->window = NULL;

    wl_display_dispatch(wl->display->display);

    wl->display->cursor.surface =
        wl_compositor_create_surface(wl->display->compositor);
}

static void destroy_display (struct wl_priv *wl)
{
    wl_surface_destroy(wl->display->cursor.surface);

    if (wl->display->cursor.theme)
        wl_cursor_theme_destroy(wl->display->cursor.theme);

    if (wl->display->shell)
        wl_shell_destroy(wl->display->shell);

    if (wl->display->compositor)
        wl_compositor_destroy(wl->display->compositor);

    if (wl->display->output)
        wl_output_destroy(wl->display->output);

    wl_display_flush(wl->display->display);
    wl_display_disconnect(wl->display->display);
    free(wl->display);
    wl->display = NULL;

    vo_fs = VO_FALSE;
}

static void create_window (struct wl_priv *wl, int width, int height)
{
    if (wl->window)
        return;

    wl->window = malloc(sizeof(struct vo_wl_window));
    wl->window->private = NULL;
    wl->window->egl_window = NULL;
    wl->window->callback = NULL;
    wl->window->width = width;
    wl->window->height = height;
    wl->window->surface = wl_compositor_create_surface(wl->display->compositor);
    wl->window->shell_surface = wl_shell_get_shell_surface(wl->display->shell,
            wl->window->surface);

    if (wl->window->shell_surface)
        wl_shell_surface_add_listener(wl->window->shell_surface,
                &shell_surface_listener, wl->window);

    wl_shell_surface_set_toplevel(wl->window->shell_surface);
}

static void destroy_window (struct wl_priv *wl)
{
    if (wl->window->callback)
        wl_callback_destroy(wl->window->callback);

    if (wl->window->egl_window)
        wl_egl_window_destroy(wl->window->egl_window);

    wl_shell_surface_destroy(wl->window->shell_surface);
    wl_surface_destroy(wl->window->surface);
    free(wl->window->private);
    free(wl->window);
    wl->window = 0;
}

static void create_input (struct wl_priv *wl)
{
    if (wl->input)
        return;

    wl->input = malloc(sizeof(struct vo_wl_input));
    wl->input->seat = NULL;
    wl->input->keyboard = NULL;
    wl->input->pointer = NULL;

    wl->input->repeat.timer_fd = timerfd_create(CLOCK_MONOTONIC,
            TFD_CLOEXEC | TFD_NONBLOCK);

    wl->input->repeat.key = 0;
    wl->input->repeat.sym = 0;
    wl->input->repeat.time = 0;

    wl->input->xkb.context = xkb_context_new(0);
    if (wl->input->xkb.context == NULL) {
        mp_msg(MSGT_VO, MSGL_ERR, "[wl] failed to initialize input.\n");
        return;
    }
}

static void destroy_input (struct wl_priv *wl)
{
    if (wl->input->seat)
        wl_seat_destroy(wl->input->seat);

    xkb_context_unref(wl->input->xkb.context);
    close(wl->input->repeat.timer_fd);
    free(wl->input);
    wl->input = NULL;
}

int vo_wl_init (struct vo *vo)
{
    struct wl_priv *wl = vo->priv;
    wl->vo = vo;
    return vo_wl_priv_init(wl);
}

int vo_wl_priv_init(struct wl_priv *wl)
{
    create_input(wl);
    create_display(wl);
    if (!wl->display) {
        mp_msg(MSGT_VO, MSGL_ERR, "[wl] failed to initialize display.\n");
        return 0;
    }

    create_window(wl, 0, 0);
    return 1;
}

void vo_wl_uninit (struct vo *vo)
{
    struct wl_priv *wl = vo->priv;
    vo_wl_priv_uninit(wl);
}

void vo_wl_priv_uninit (struct wl_priv *wl)
{
    destroy_input(wl);
    destroy_window(wl);
    destroy_display(wl);
}

void vo_wl_ontop (struct vo *vo)
{
}

void vo_wl_border (struct vo *vo)
{
}

void vo_wl_priv_fullscreen (struct wl_priv *wl)
{
    if (!wl->window || !wl->display->shell)
		return;

    if (!vo_fs) {
        wl->window->p_width = wl->window->width;
        wl->window->p_height = wl->window->height;
        wl_shell_surface_set_fullscreen(wl->window->shell_surface,
                WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE,
                0, NULL);
        wl->window->type = TYPE_FULLSCREEN;
        vo_fs = VO_TRUE;
    } else {
        wl_shell_surface_set_toplevel(wl->window->shell_surface);
        wl->window->width = wl->window->p_width;
        wl->window->height = wl->window->p_height;
        wl->window->type = TYPE_TOPLEVEL;
        vo_fs = VO_FALSE;
    }
}

void vo_wl_fullscreen (struct vo *vo)
{
    struct wl_priv *wl = vo->priv;
    vo_wl_priv_fullscreen(wl);
}

int vo_wl_priv_check_events (struct wl_priv *wl)
{
    int ret = 0;
    uint64_t exp;
    wl->input->events = 0;
    wl_display_roundtrip(wl->display->display);

    if (read(wl->input->repeat.timer_fd, &exp, sizeof exp) == sizeof exp) {
        keyboard_handle_key(wl, wl->input->keyboard, 0, wl->input->repeat.time,
                wl->input->repeat.key, WL_KEYBOARD_KEY_STATE_PRESSED);
    }

    ret = wl->input->events;
    return ret;
}

int vo_wl_check_events (struct vo *vo)
{
    struct wl_priv *wl = vo->priv;
    return vo_wl_priv_check_events(wl);
}

