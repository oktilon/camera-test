/**
 * @file  app.c
 * @brief Main application for Camera test
 *
 * @date  2026-06-08
 * @author Denys Stovbun
 */
#include <stdio.h>
#include <gtk-3.0/gtk/gtk.h>
#include <glib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <linux/videodev2.h>
#include <unistd.h>

#include "main.h"
#include "app.h"

/* Wrapper that retries ioctl on EINTR */
static int xioctl(int fd, unsigned long request, void *arg) {
    int r;
    do { r = ioctl(fd, request, arg); } while (r == -1 && errno == EINTR);
    return r;
}

/**
 * @brief Clamp value to byte size
 *
 * @param v value
 * @return uint8_t value clamped to byte size
 */
static inline uint8_t clamp_u8(int v) {
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/**
 * @brief NV12 to RGB converter with rescaling
 *
 * @param y_plane   NV12 Src Luma (Y) plane (B/W image)
 * @param uv_plane  NV12 Src Chroma (UV) plane (Color info)
 * @param dst       Dst RGB buffer (3 bytes per pixel)
 * @param src_w     Src frame width
 * @param src_h     Src frame height
 * @param dst_w     Dst frame width
 * @param dst_h     Dst frame height
 * @param rowstride Dst row size aligned in pixels
 */
static void nv12_to_rgb_scaled(const uint8_t *y_plane,
                        const uint8_t *uv_plane,
                        uint8_t       *dst,
                        int src_w, int src_h,
                        int dst_w, int dst_h,
                        int rowstride)
{
    /* Fixed-point step: how many source pixels per dest pixel */
    int step_x = (src_w << 16) / dst_w;   // 16-bit fractional
    int step_y = (src_h << 16) / dst_h;

    int src_y_fp = 0;  // current source row in fixed-point
    for (int dy = 0; dy < dst_h; dy++, src_y_fp += step_y) {

        int src_row = src_y_fp >> 16;          // integer source row
        int src_uv_row = (src_row / 2) * src_w;

        const uint8_t *y_row  = y_plane  + src_row * src_w;
        const uint8_t *uv_row = uv_plane + src_uv_row;

        uint8_t *out = dst + dy * rowstride;

        int src_x_fp = 0;
        for (int dx = 0; dx < dst_w; dx++, src_x_fp += step_x) {

            int src_col = src_x_fp >> 16;

            int y = y_row[src_col];
            int u = uv_row[(src_col & ~1)    ] - 128;
            int v = uv_row[(src_col & ~1) + 1] - 128;

            *out++ = clamp_u8(y + (359 * v >> 8));
            *out++ = clamp_u8(y - (88 * u >> 8) - (183 * v >> 8));
            *out++ = clamp_u8(y + (454 * u >> 8));
        }
    }
}

/**
 * @brief Timer callback: Query frame from device and display it
 *
 * @param data AppState pointer
 * @return gboolean
 */
static gboolean on_frame_timeout(gpointer data) {
    AppState *app = (AppState *)data;

    /* Use select() to check if a frame is ready without blocking */
    fd_set fds;
    struct timeval tv = {0, 0};   /* non-blocking poll */
    FD_ZERO(&fds);
    FD_SET(app->fd, &fds);
    int r = select(app->fd + 1, &fds, NULL, NULL, &tv);
    if (r == -1) {
        if (errno == EINTR) return G_SOURCE_CONTINUE;
        log_message("select");
        return G_SOURCE_REMOVE;
    }
    if (r == 0)
        return G_SOURCE_CONTINUE;   /* no frame yet — try again next tick */

    /* Dequeue the filled buffer */
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(planes, 0, sizeof(planes));
    struct v4l2_buffer buf = {0};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = NUM_PLANES;
    buf.m.planes = planes;

    if (xioctl(app->fd, VIDIOC_DQBUF, &buf) == -1) {
        if (errno == EAGAIN) return G_SOURCE_CONTINUE;
        log_message("VIDIOC_DQBUF");
        return G_SOURCE_REMOVE;
    }

    /* skip every 3 buffers of 4 */
    if (buf.index == 0) {
        /* Convert NV12 to RGB directly into the GdkPixbuf pixel data */
        guchar *pixels   = gdk_pixbuf_get_pixels(app->pixbuf);
        int     rowstride = gdk_pixbuf_get_rowstride(app->pixbuf);

        nv12_to_rgb_scaled((uint8_t *)app->buffers[buf.index].plane[0].start,
                        (uint8_t *)app->buffers[buf.index].plane[1].start,
                            pixels,
                            app->cap_width, app->cap_height,
                            app->disp_width, app->disp_height,
                            rowstride);

        /* Redraw Drawing Area */
        gtk_widget_queue_draw(app->drawing_area);
    }

    /* Re-queue the buffer so the kernel can refill it */
    memset(planes, 0, sizeof(planes));
    buf.m.planes = planes;
    if (xioctl(app->fd, VIDIOC_QBUF, &buf) == -1) {
        log_message("VIDIOC_QBUF (requeue)");
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE; // Return TRUE to continue calling this function periodically
}

/**
 * @brief Gracefully close all handlers
 *
 * @param app AppState pointer
 */
void app_cleanup(AppState *app) {
    /* Stop streaming */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(app->fd, VIDIOC_STREAMOFF, &type);

    /* Unmap buffers */
    for (guint i = 0; i < app->n_buffers; i++)
        if (app->buffers[i].plane[0].start != MAP_FAILED)
            munmap(app->buffers[i].plane[0].start, app->buffers[i].plane[0].length);
    /* Close device */
    close(app->fd);
    /* Free image memory */
    if (app->pixbuf && G_IS_OBJECT(app->pixbuf)) {
        g_object_unref(app->pixbuf);
    }
}

/**
 * @brief Close application handler
 *
 * @param widget (Unused)
 * @param user_data AppState pointer
 */
static void on_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppState *app = (AppState *)user_data;
    app_cleanup(app);
    gtk_main_quit();
}

/**
 * @brief Function to redraw DrawingArea control
 * Just copy PixBuffer into DrawingArea
 *
 * @param widget Pointer to DrawingArea widget
 * @param cr Pointer to Cairo Drawing Context for Drawing Area
 * @param user_data AppState pointer
 * @return gboolean TRUE - stop event handling, FALSE - propagate event to next handler
 */
static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    (void)widget;
    AppState *app = (AppState *)user_data;

    if (!app->pixbuf) return FALSE;

    gdk_cairo_set_source_pixbuf(cr, app->pixbuf, 0, 0);
    cairo_paint(cr);

    return FALSE;
}

/**
 * @brief Create and display UI
 *
 * @param app AppState pointer
 */
void app_ui(AppState *app) {
    // Main window
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "");
    gtk_window_set_default_size(GTK_WINDOW(app->window),
                                700, 1000);

    gtk_window_set_decorated(GTK_WINDOW(app->window), FALSE);
    gtk_window_fullscreen(GTK_WINDOW(app->window));

    /* GdkPixbuf that we'll write raw RGB into each frame.
     * has_alpha=FALSE, bits_per_sample=8 → 3 bytes per pixel */
    app->pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB,
                                  FALSE, 8,
                                  app->disp_width, app->disp_height);
    if (!app->pixbuf) {
        log_message("Failed to create GdkPixbuf");
        exit(1);
    }

    // Drawing area for camera frames
    app->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(app->drawing_area, app->disp_width, app->disp_height);
    g_signal_connect(app->drawing_area, "draw", G_CALLBACK(on_draw), app);
    gtk_widget_set_halign(app->drawing_area, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(app->drawing_area, GTK_ALIGN_CENTER);

    // Close button
    GtkWidget *buttonClose = gtk_button_new_with_label("Close");
    g_signal_connect(buttonClose, "clicked", G_CALLBACK(on_destroy), app);
    gtk_widget_set_size_request(buttonClose, 150, 100);
    gtk_widget_set_halign(app->drawing_area, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(app->drawing_area, GTK_ALIGN_CENTER);

    // Empty labels to center vertically
    GtkWidget *spcTop = gtk_label_new("");
    gtk_widget_set_vexpand(spcTop, TRUE);

    GtkWidget *spcBot = gtk_label_new("");
    gtk_widget_set_vexpand(spcBot, TRUE);

    // Box layout
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(box), spcTop, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), app->drawing_area, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), buttonClose, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), spcBot, TRUE, TRUE, 0);

    // Add box to window
    gtk_container_add(GTK_CONTAINER(app->window), box);

    g_signal_connect(app->window, "destroy", G_CALLBACK(on_destroy), app);

    // Display window with all it's components
    gtk_widget_show_all(app->window);

    // Register the frame-fetch callback with GLib's main loop
    g_timeout_add(FRAME_INTERVAL_MS, on_frame_timeout, app);
}

/**
 * @brief Check file is character device and open it
 *
 * @param dev_name Device path
 * @return int File descriptor
 */
static int v4l2_open_device(const char *dev_name) {
    struct stat st;
    if (stat(dev_name, &st) == -1) {
        log_message("Cannot stat '%s': %s", dev_name, strerror(errno));
        return -1;
    }
    if (!S_ISCHR(st.st_mode)) {
        log_message("'%s' is not a character device", dev_name);
        return -1;
    }

    int fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);
    if (fd == -1) {
        log_message("Cannot open '%s': %s", dev_name, strerror(errno));
        return -1;
    }
    return fd;
}

/**
 * @brief Initialize application:
 * 1. Open device
 * 2. Check device capabilities to stream
 * 3. Set frame format
 * 4. Request and setup frame buffers
 * 5. Start stream
 *
 * @param app AppState pointer
 * @param dev_name Device path
 * @return int State: 0 = Ok, -1 = Error
 */
int app_init(AppState *app, const char *dev_name) {
    // Open device
    app->fd = v4l2_open_device(dev_name);
    if (app->fd == -1) return -1;

    // Query and check capabilities
    struct v4l2_capability cap = {0};
    if (xioctl(app->fd, VIDIOC_QUERYCAP, &cap) == -1) {
        log_message("VIDIOC_QUERYCAP");
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) && !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        log_message("Device '%s' does not support video capture", dev_name);
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        log_message("Device '%s' does not support streaming", dev_name);
        return -1;
    }
    printf("Driver : %s\nCard   : %s\nBus    : %s\n",
           cap.driver, cap.card, cap.bus_info);

    // Set frame format
    struct v4l2_format fmt = {0};
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width       = CAPTURE_WIDTH;
    fmt.fmt.pix_mp.height      = CAPTURE_HEIGHT;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
    // NV12 has 2 planes: Y plane (full resolution) and interleaved UV plane (half resolution)
    fmt.fmt.pix_mp.num_planes  = 2;
    // NV12 plane 0 full resolution (Luma Y)
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = CAPTURE_WIDTH * CAPTURE_HEIGHT;
    fmt.fmt.pix_mp.plane_fmt[0].bytesperline = CAPTURE_WIDTH;
    // NV12 plane 1 half resolution (Chroma UV)
    fmt.fmt.pix_mp.plane_fmt[1].sizeimage = CAPTURE_WIDTH * CAPTURE_HEIGHT / 2;
    fmt.fmt.pix_mp.plane_fmt[1].bytesperline = CAPTURE_WIDTH;

    if (xioctl(app->fd, VIDIOC_S_FMT, &fmt) == -1) {
        log_message("VIDIOC_S_FMT %d", errno);
        return -1;
    }

    // Check applied format
    struct v4l2_format fmt_check = {0};
    fmt_check.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(app->fd, VIDIOC_G_FMT, &fmt_check);

    printf("Colorspace : %d  (1=SMPTE170M, 7=JPEG/full-range)\n",
                fmt_check.fmt.pix_mp.colorspace);
    printf("Quantization: %d (0=default, 1=full-range, 2=limited)\n",
                fmt_check.fmt.pix_mp.quantization);
    app->cap_width  = fmt.fmt.pix_mp.width;
    app->cap_height = fmt.fmt.pix_mp.height;
    int y_size  = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    int uv_size  = fmt.fmt.pix_mp.plane_fmt[1].sizeimage;
    printf("Format : %dx%d, fourcc=%.4s [Ysz=%d, UVsz=%d]\n",
           app->cap_width, app->cap_height,
           (char *)&fmt.fmt.pix_mp.pixelformat,
        y_size, uv_size);

    // Request MMAP multi-planar buffers
    struct v4l2_requestbuffers req = {0};
    req.count  = N_BUFFERS;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(app->fd, VIDIOC_REQBUFS, &req) == -1) {
        log_message("VIDIOC_REQBUFS");
        return -1;
    }
    if (req.count < 2) {
        log_message("Insufficient buffer memory");
        return -1;
    }
    app->n_buffers = req.count;

    // MMAP each plane of each buffer
    for (guint i = 0; i < app->n_buffers; i++) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
        struct v4l2_buffer buf = {0};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        buf.length = NUM_PLANES;
        buf.m.planes = planes;

        // Query NUM_PLANES for buffer #i
        if (xioctl(app->fd, VIDIOC_QUERYBUF, &buf) == -1) {
            log_message("VIDIOC_QUERYBUF(%d) %s", errno, strerror(errno));
            return -1;
        }

        // mmap all planes
        for (int p = 0; p < NUM_PLANES; p++) {
            app->buffers[i].plane[p].length = planes[p].length;
            app->buffers[i].plane[p].start  = mmap(NULL,
                                        planes[p].length,
                                        PROT_READ | PROT_WRITE,
                                        MAP_SHARED,
                                        app->fd,
                                        planes[p].m.mem_offset);
            if (app->buffers[i].plane[p].start == MAP_FAILED) {
                log_message("mmap(%d): %s", errno, strerror(errno));
                return -1;
            }
        }

    }

    // Queue all buffers
    for (guint i = 0; i < app->n_buffers; i++) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(planes, 0, sizeof(planes));
        struct v4l2_buffer buf = {0};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        buf.length = NUM_PLANES;
        buf.m.planes = planes;

        if (xioctl(app->fd, VIDIOC_QBUF, &buf) == -1) {
            log_message("VIDIOC_QBUF (init)");
            return -1;
        }

    }

    // Start streaming
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(app->fd, VIDIOC_STREAMON, &type) == -1) {
        log_message("VIDIOC_STREAMON (%d): %s", errno, strerror(errno));
        return -1;
    }

    return 0;
}