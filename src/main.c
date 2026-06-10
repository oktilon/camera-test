/**
 * @file main.c
 * @brief Minimal Wayland SHM client for camera display.
 * 
 * @author Denys Stovbun
 * @date 2026-06-09
 *
 */

#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "main.h"

static AppState g_app;

/***************************************************************************
 * Helpers
 ****************************************************************************/

static int xioctl(int fd, unsigned long req, void *arg) {
    int r; do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static inline uint8_t clamp_u8(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/*
 * NV12 to XRGB8888 with nearest-neighbour scale
 * Wayland wl_shm uses ARGB8888 (little-endian: B G R X in memory)
 */
static void nv12_to_xrgb_scaled(const uint8_t *y_plane,
                                  const uint8_t *uv_plane,
                                  uint32_t      *dst,       /* XRGB8888     */
                                  int src_w, int src_h,
                                  int dst_w, int dst_h)
{
    int step_x = (src_w << 16) / dst_w;
    int step_y = (src_h << 16) / dst_h;

    int src_y_fp = 0;
    for (int dy = 0; dy < dst_h; dy++, src_y_fp += step_y) {
        int src_row    = src_y_fp >> 16;
        int src_uv_row = (src_row / 2) * src_w;

        const uint8_t *y_row  = y_plane  + src_row * src_w;
        const uint8_t *uv_row = uv_plane + src_uv_row;

        uint32_t *out = dst + dy * dst_w;

        int src_x_fp = 0;
        for (int dx = 0; dx < dst_w; dx++, src_x_fp += step_x) {
            int src_col = src_x_fp >> 16;

            int y = y_row[src_col];
            int u = uv_row[(src_col & ~1)    ] - 128;
            int v = uv_row[(src_col & ~1) + 1] - 128;

            uint8_t r = clamp_u8(y + (359 * v >> 8));
            uint8_t g = clamp_u8(y - (88  * u >> 8) - (183 * v >> 8));
            uint8_t b = clamp_u8(y + (454 * u >> 8));

            /* XRGB8888 little-endian: byte order B G R X */
            *out++ = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

/***************************************************************************
 * Wayland registry
 ****************************************************************************/

static void registry_global(void *data, struct wl_registry *reg,
                             uint32_t name, const char *interface,
                             uint32_t version)
{
    WlState *wl = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        wl->compositor = wl_registry_bind(reg, name,
                                          &wl_compositor_interface, 1);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        wl->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
        wl->xdg_wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
}

static void registry_global_remove(void *data, struct wl_registry *reg,
                                    uint32_t name) { (void)data;(void)reg;(void)name; }

static const struct wl_registry_listener registry_listener = {
    registry_global, registry_global_remove
};

/* xdg_wm_base ping/pong required or Weston kills your client */
static void xdg_wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { xdg_wm_base_ping };

/* xdg_surface configure must ack before first commit */
static void xdg_surface_configure(void *data, struct xdg_surface *surf, uint32_t serial) {
    WlState *wl = data;
    xdg_surface_ack_configure(surf, serial);
    wl->configured = 1;
}
static const struct xdg_surface_listener xdg_surface_listener = { xdg_surface_configure };

/* xdg_toplevel configure/close */
static void xdg_toplevel_configure(void *data, struct xdg_toplevel *tl,
                                    int32_t w, int32_t h,
                                    struct wl_array *states) { /* ignore resize */ }
static void xdg_toplevel_close(void *data, struct xdg_toplevel *tl) {
    g_app.running = 0;
}
static const struct xdg_toplevel_listener toplevel_listener = {
    xdg_toplevel_configure, xdg_toplevel_close
};

/***************************************************************************
 * Wayland SHM setup
 ****************************************************************************/

static int create_shm_fd(int size) {
    /* Use memfd if available, fall back to shm_open */
    int fd = -1;

#ifdef __NR_memfd_create
    fd = syscall(__NR_memfd_create, "camera_shm", 0);
#endif
    if (fd < 0) {
        char name[64];
        snprintf(name, sizeof(name), "/camera_shm_%d", getpid());
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) shm_unlink(name);
    }

    if (fd < 0) { perror("shm fd"); return -1; }
    if (ftruncate(fd, size) < 0) { perror("ftruncate"); close(fd); return -1; }
    return fd;
}

static int wayland_init(WlState *wl, int disp_w, int disp_h) {
    wl->display = wl_display_connect(NULL);
    if (!wl->display) { fprintf(stderr, "Cannot connect to Wayland\n"); return -1; }

    wl->registry = wl_display_get_registry(wl->display);
    wl_registry_add_listener(wl->registry, &registry_listener, wl);
    wl_display_roundtrip(wl->display);  /* collect globals */

    if (!wl->compositor || !wl->shm) {
        fprintf(stderr, "Missing Wayland globals (compositor=%p shm=%p)\n",
                wl->compositor, wl->shm);
        return -1;
    }

    /* Shared memory: XRGB8888 = 4 bytes per pixel */
    wl->shm_size = disp_w * disp_h * 4;
    wl->shm_fd   = create_shm_fd(wl->shm_size);
    if (wl->shm_fd < 0) return -1;

    wl->shm_data = mmap(NULL, wl->shm_size,
                        PROT_READ | PROT_WRITE, MAP_SHARED, wl->shm_fd, 0);
    if (wl->shm_data == MAP_FAILED) { perror("mmap shm"); return -1; }

    wl->pool   = wl_shm_create_pool(wl->shm, wl->shm_fd, wl->shm_size);
    wl->buffer = wl_shm_pool_create_buffer(wl->pool, 0,
                                            disp_w, disp_h,
                                            disp_w * 4,           /* stride */
                                            WL_SHM_FORMAT_XRGB8888);

    xdg_wm_base_add_listener(wl->xdg_wm_base, &wm_base_listener, wl);

    wl->surface       = wl_compositor_create_surface(wl->compositor);
    wl->xdg_surface   = xdg_wm_base_get_xdg_surface(wl->xdg_wm_base, wl->surface);
    xdg_surface_add_listener(wl->xdg_surface, &xdg_surface_listener, wl);

    wl->xdg_toplevel = xdg_surface_get_toplevel(wl->xdg_surface);
    xdg_toplevel_add_listener(wl->xdg_toplevel, &toplevel_listener, wl);
    xdg_toplevel_set_title(wl->xdg_toplevel, "Camera");

    /* Commit to trigger the configure event */
    wl_surface_commit(wl->surface);
    wl_display_roundtrip(wl->display);   /* wait for configured=1 */

    /* Now safe to attach the buffer */
    wl_surface_attach(wl->surface, wl->buffer, 0, 0);
    wl_surface_damage(wl->surface, 0, 0, disp_w, disp_h);
    wl_surface_commit(wl->surface);
    wl_display_flush(wl->display);

    return 0;
}

static void wayland_present_frame(WlState *wl, int disp_w, int disp_h) {
    /* shm_data already filled by nv12_to_xrgb_scaled */
    wl_surface_attach(wl->surface, wl->buffer, 0, 0);
    wl_surface_damage(wl->surface, 0, 0, disp_w, disp_h);
    wl_surface_commit(wl->surface);
    wl_display_flush(wl->display);

    /* Dispatch any incoming Wayland events (resize, close, etc.) */
    wl_display_dispatch_pending(wl->display);
}

static void wayland_cleanup(WlState *wl, int disp_w, int disp_h) {
    if (wl->buffer)        wl_buffer_destroy(wl->buffer);
    if (wl->pool)          wl_shm_pool_destroy(wl->pool);
    if (wl->shm_data)      munmap(wl->shm_data, disp_w * disp_h * 4);
    if (wl->shm_fd >= 0)   close(wl->shm_fd);
    if (wl->shell_surface) wl_shell_surface_destroy(wl->shell_surface);
    if (wl->surface)       wl_surface_destroy(wl->surface);
    // if (wl->shell)         wl_shell_destroy(wl->shell);
    if (wl->shm)           wl_shm_destroy(wl->shm);
    if (wl->compositor)    wl_compositor_destroy(wl->compositor);
    if (wl->registry)      wl_registry_destroy(wl->registry);
    if (wl->display)       wl_display_disconnect(wl->display);
}

/***************************************************************************
 * V4L2 init/cleanup (MPLANE / NV12 — same as your existing code)
 ****************************************************************************/

static int cam_init(AppState *app, const char *dev) {
    app->cam_fd = open(dev, O_RDWR | O_NONBLOCK);
    if (app->cam_fd < 0) { perror("open"); return -1; }

    struct v4l2_format fmt = {0};
    fmt.type                   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width       = CAPTURE_WIDTH;
    fmt.fmt.pix_mp.height      = CAPTURE_HEIGHT;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.num_planes  = NUM_PLANES;
    xioctl(app->cam_fd, VIDIOC_S_FMT, &fmt);
    app->cap_w = fmt.fmt.pix_mp.width;
    app->cap_h = fmt.fmt.pix_mp.height;

    struct v4l2_requestbuffers req = {0};
    req.count  = N_BUFFERS;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    xioctl(app->cam_fd, VIDIOC_REQBUFS, &req);
    app->n_buffers = req.count;

    for (int i = 0; i < app->n_buffers; i++) {
        struct v4l2_plane planes[2] = {0};
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i; buf.length = NUM_PLANES; buf.m.planes = planes;
        xioctl(app->cam_fd, VIDIOC_QUERYBUF, &buf);
        for (int p = 0; p < NUM_PLANES; p++) {
            app->buffers[i].plane[p].length = planes[p].length;
            app->buffers[i].plane[p].start  = mmap(NULL, planes[p].length,
                PROT_READ|PROT_WRITE, MAP_SHARED,
                app->cam_fd, planes[p].m.mem_offset);
        }
    }

    for (int i = 0; i < app->n_buffers; i++) {
        struct v4l2_plane planes[2] = {0};
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP; buf.index = i;
        buf.length = NUM_PLANES; buf.m.planes = planes;
        xioctl(app->cam_fd, VIDIOC_QBUF, &buf);
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(app->cam_fd, VIDIOC_STREAMON, &type);
    return 0;
}

static void cam_cleanup(AppState *app) {
    if (app->cam_fd < 0) return;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(app->cam_fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < app->n_buffers; i++)
        for (int p = 0; p < NUM_PLANES; p++)
            if (app->buffers[i].plane[p].start != MAP_FAILED)
                munmap(app->buffers[i].plane[p].start,
                       app->buffers[i].plane[p].length);
    close(app->cam_fd);
    app->cam_fd = -1;
}

/***************************************************************************
 * Main loop
 ****************************************************************************/

static void on_signal(int sig) { (void)sig; g_app.running = 0; }

int main(void) {
    long frames = 0;
    g_app.running = 1;
    g_app.disp_w  = DISP_WIDTH;
    g_app.disp_h  = DISP_HEIGHT;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    if (cam_init(&g_app, "/dev/video0") < 0) return 1;
    if (wayland_init(&g_app.wl, DISP_WIDTH, DISP_HEIGHT) < 0) return 1;

    printf("Streaming %dx%d => %dx%d via Wayland SHM\nCaptured frames: 0\n",
           CAPTURE_WIDTH, CAPTURE_HEIGHT, DISP_WIDTH, DISP_HEIGHT);

    while (g_app.running) {
        /* Wait for camera frame — blocking select with 100ms timeout */
        fd_set fds;
        struct timeval tv = {0, 100000};
        FD_ZERO(&fds); FD_SET(g_app.cam_fd, &fds);
        int r = select(g_app.cam_fd + 1, &fds, NULL, NULL, &tv);
        if (r <= 0) continue;

        struct v4l2_plane planes[2] = {0};
        struct v4l2_buffer buf = {0};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = NUM_PLANES; buf.m.planes = planes;

        if (xioctl(g_app.cam_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            perror("DQBUF"); break;
        }

        /* Convert directly into Wayland SHM — zero extra copy */
        nv12_to_xrgb_scaled(
            g_app.buffers[buf.index].plane[0].start,
            g_app.buffers[buf.index].plane[1].start,
            (uint32_t *)g_app.wl.shm_data,
            g_app.cap_w, g_app.cap_h,
            g_app.disp_w, g_app.disp_h);

        wayland_present_frame(&g_app.wl, DISP_WIDTH, DISP_HEIGHT);
        frames++;

        memset(planes, 0, sizeof(planes));
        buf.m.planes = planes;
        xioctl(g_app.cam_fd, VIDIOC_QBUF, &buf);

        printf("\033[1F\033[18G%ld\n", frames);
        fflush(stdout);

    }

    cam_cleanup(&g_app);
    wayland_cleanup(&g_app.wl, DISP_WIDTH, DISP_HEIGHT);
    return 0;
}

