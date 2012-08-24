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
 *
 * You can alternatively redistribute this file and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "config.h"
#include "mp_msg.h"
#include "video_out.h"
#include "talloc.h"
#include "fastmemcpy.h"

#include "libmpcodecs/vfcap.h"
#include "libmpcodecs/mp_image.h"

#include "wl_common.h"

#define clamp255(val) \
    (val) = ((val) < 0 ? 0 : ((val) > 255 ? 255 : (val)))

/* Tempfile: copied from weston - shared/os-compatibility */
static int set_cloexec_or_close(int fd);
static int create_tmpfile_cloexec(char *tmpname);
int os_create_anonymous_file(off_t size);

static int
set_cloexec_or_close(int fd)
{
    long flags;

    if (fd == -1)
        return -1;

    flags = fcntl(fd, F_GETFD);
    if (flags == -1)
        goto err;

    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
        goto err;

    return fd;

err:
    close(fd);
    return -1;
}

static int
create_tmpfile_cloexec(char *tmpname)
{
    int fd;

#ifdef HAVE_MKOSTEMP
    fd = mkostemp(tmpname, O_CLOEXEC);
    if (fd >= 0)
        unlink(tmpname);
#else
    fd = mkstemp(tmpname);
    if (fd >= 0) {
        fd = set_cloexec_or_close(fd);
        unlink(tmpname);
    }
#endif

    return fd;
}

int os_create_anonymous_file(off_t size)
{
    static const char template[] = "/weston-shared-XXXXXX";
    const char *path;
    char *name;
    int fd;

    path = getenv("XDG_RUNTIME_DIR");
    if (!path) {
        errno = ENOENT;
        return -1;
    }

    name = malloc(strlen(path) + sizeof(template));
    if (!name)
        return -1;

    strcpy(name, path);
    strcat(name, template);

    fd = create_tmpfile_cloexec(name);

    free(name);

    if (fd < 0)
        return -1;

    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/* SHM Specific */
struct vo_wl_private {
    int width, height;
    struct wl_buffer * buffer;
    void *shm_data;
};

static struct wl_buffer *
create_shm_buffer (struct wl_priv * wl, int width, int height,
        uint32_t format)
{
    struct wl_shm_pool *pool;
    struct wl_buffer *buffer;
    int fd, size, stride;
    void *data;

    stride = width * 4;
    size = stride * height;

    fd = os_create_anonymous_file(size);

    if (fd < 0) {
        /* mplayer message */
        return NULL;
    }

    data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        /* mplayer message */
        close(fd);
        return NULL;
    }

    pool = wl_shm_create_pool(wl->display->shm, fd, size);
    buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
            stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);

    wl->window->private->shm_data = data;

    return buffer;
}

/* REDRAW CALLBACK: */
static const struct wl_callback_listener frame_listener;

static void window_redraw (void *data, struct wl_callback *callback, uint32_t time)
{
    struct vo_wl_window *window = data;

    wl_surface_attach(window->surface, window->private->buffer, 0, 0);
    wl_surface_damage(window->surface, 0, 0, window->width, window->height);

    if (callback)
        wl_callback_destroy(callback);

    window->callback = wl_surface_frame(window->surface);
    wl_callback_add_listener(window->callback, &frame_listener, window);
}

static const struct wl_callback_listener frame_listener = {
    window_redraw
};
/* END REDRAW */

static int query_format(struct vo *vo, uint32_t format)
{
    int caps = VFCAP_CSP_SUPPORTED | VFCAP_FLIP | VFCAP_ACCEPT_STRIDE;
    return caps;
}

static uint32_t get_image(struct vo *vo, mp_image_t *mpi)
{
    return 0;
}

static int draw_slice(struct vo *vo, uint8_t *image[], int stride[],
        int w, int h, int x, int y);

static uint32_t draw_image(struct vo *vo, mp_image_t *mpi)
{
    struct wl_priv *wl = vo->priv;

    if (mpi->flags & MP_IMGFLAG_DRAW_CALLBACK)
        ; // done
    else if (mpi->flags & MP_IMGFLAG_PLANAR)
        draw_slice(vo, mpi->planes, mpi->stride, mpi->w, mpi->h, 0, 0);
    else if (mpi->flags & MP_IMGFLAG_YUV)
        ; // packed YUV:
    else
          return false;

    return true;
}

/* VIDEO OUT DRIVER FUNCTIONS */
static int
preinit (struct vo *vo, const char *arg)
{
    struct wl_priv * wl = talloc_zero(vo, struct wl_priv);

    vo->priv = wl;

    if(arg)
    {
        mp_tmsg(MSGT_VO,MSGL_WARN, "[wl] Unknown subdevice: %s.\n",arg);
        return ENOSYS;
    }

    vo_wl_init(vo);

    wl->window->private = malloc(sizeof(struct vo_wl_private));

    return 0;
}

static int
config (struct vo *vo, uint32_t width, uint32_t height,
        uint32_t d_width, uint32_t d_height,
        uint32_t fullscreen, uint32_t format)
{
    struct wl_priv *wl = vo->priv;

    switch(format) {
        case IMGFMT_I420:
            break;
        case IMGFMT_YV12:
        case IMGFMT_IYUV:
        case IMGFMT_YUY2:
        case IMGFMT_UYVY:
        case IMGFMT_YVYU:
            break;
        case IMGFMT_BGR15:
        case IMGFMT_BGR16:
        case IMGFMT_BGR24:
        case IMGFMT_BGR32:
            break;
        case IMGFMT_RGB15:
        case IMGFMT_RGB16:
        case IMGFMT_RGB24:
        case IMGFMT_RGB32:
            break;
        default:
             mp_tmsg(MSGT_VO,MSGL_WARN,
                     "[wl] Unsupported image format (0x%X).\n"
                    ,format);
    }

    wl->window->width = width;
    wl->window->height = height;
    wl->window->p_width = width;
    wl->window->p_height = height;

    wl->window->private->buffer = create_shm_buffer(wl, width, height,
            WL_SHM_FORMAT_XRGB8888);
#if 0
    if (!(wl->display->formats & (1 << WL_SHM_FORMAT_XRGB8888))) {
        mp_msg(MSGT_VO, MSGL_ERR, "[wl] WL_SHM_FORMAT_XRGB8888 not available\n");
        vo_wl_uninit(vo);
        return -1;
    }
#endif

    window_redraw(wl->window, NULL, 0);
    return 0;
}

static int
control (struct vo *vo, uint32_t request, void *data)
{
    struct wl_priv *wl = vo->priv;

    switch (request) {
        case VOCTRL_QUERY_FORMAT:
            return query_format(vo, *(uint32_t *)data);
        case VOCTRL_GET_IMAGE:
            break;
        case VOCTRL_DRAW_IMAGE:
            draw_image(vo, (mp_image_t *)data);
            return VO_TRUE;
        case VOCTRL_DRAW_EOSD:
            break;
        case VOCTRL_GET_EOSD_RES:
            break;
        case VOCTRL_ONTOP:
            vo_wl_ontop(vo);
            return VO_TRUE;
        case VOCTRL_FULLSCREEN:
            vo_wl_fullscreen(vo);
            return VO_TRUE;
        case VOCTRL_BORDER:
        case VOCTRL_GET_PANSCAN:
        case VOCTRL_SET_PANSCAN:
        case VOCTRL_GET_EQUALIZER:
        case VOCTRL_SET_EQUALIZER:
        case VOCTRL_SET_YUV_COLORSPACE:
        case VOCTRL_GET_YUV_COLORSPACE:
        case VOCTRL_UPDATE_SCREENINFO:
            break;
        case VOCTRL_REDRAW_FRAME:
            wl_surface_damage(wl->window->surface, 0, 0,
                    wl->window->width, wl->window->height);
            wl_display_flush(wl->display->display);
            return VO_TRUE;
        case VOCTRL_SCREENSHOT:
            break;

    }
    return VO_NOTIMPL;
}

static void
get_buffered_frame (struct vo *vo, bool eof)
{
}

static int
draw_slice (struct vo *vo, uint8_t *image[], int stride[],
        int w, int h, int x, int y)
{
    struct wl_priv *wl = vo->priv;

    uint32_t *dest = wl->window->private->shm_data;
    uint8_t *dest8;

    uint32_t i, chr;
    uint32_t lines, column;
    int16_t _y, _u, _v;
    int16_t r, g, b;

    for (i = 0, chr = 0, lines  = 0, column = 0; i < w*h; ++i, ++column) {

        if (column == stride[0]) {
            lines++;
            column = 0;
        }

        chr = ((column) >> 1) + (stride[1] * (lines >> 1));

        _y = *(image[0] + i) - 16;
        _u = *(image[1] + chr) - 128;
        _v = *(image[2] + chr) - 128;

        r = (298 * (_y) + 409 * (_v) + 128) >> 8;
        g = (298 * (_y) - 100 * (_u) - 208 * (_v) + 128) >> 8;
        b = (298 * (_y) + 516 * (_u) + 128) >> 8;
#if 0
        r = (_y - 16) * 1.164 + 1.596 * (_v - 128);
        g = (_y - 16) * 1.164 - 0.813 * (_v - 128) - 0.391 * (_u - 128);
        b = (_y - 16) * 1.164                      + 2.018 * (_u - 128);
#endif
        dest8 = (uint8_t *)dest++;
        *dest8 = clamp255(b);
        *(dest8+1) = clamp255(g);
        *(dest8+2) = clamp255(r);
        *(dest8+3) = 0xFF;
    }

    return 0;
}

static void
draw_osd(struct vo *vo, struct osd_state *osd)
{
}

static void
flip_page(struct vo *vo)
{
    struct wl_priv *wl = vo->priv;
    wl_display_iterate(wl->display->display, wl->display->mask);
}

static void
check_events (struct vo *vo)
{
}

static void
uninit (struct vo *vo)
{
    struct wl_priv *wl = vo->priv;
    wl_buffer_destroy(wl->window->private->buffer);
    free(wl->window->private->shm_data);
    free(wl->window->private);
    vo_wl_uninit(vo);
}

const struct vo_driver video_out_wl = {
    .is_new = true,
    .info = &(const vo_info_t) {
        "Wayland / SHM",
        "wl",
        "Alexander Preisinger <alexander.preisinger@gmail.com",
        ""
    },
    .preinit = preinit,
    .config = config,
    .control = control,
    .draw_slice = draw_slice,
    .draw_osd = draw_osd,
    .flip_page = flip_page,
    .check_events = check_events,
    .uninit = uninit,
};

