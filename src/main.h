/**
 * @file    main.h
 * @brief   Main header
 * 
 * @fdate   2026-06-08
 * @author  Denys Stovbun
 */

#ifndef MAIN_H_
#define MAIN_H_

#include <gtk-3.0/gtk/gtk.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <unistd.h>

#include "config.h"

#define IMAGE_WIDTH 640
#define IMAGE_HEIGHT 360
#define CAPTURE_WIDTH 1280
#define CAPTURE_HEIGHT 720
#define FRAME_INTERVAL_MS 33
#define N_BUFFERS 4

// NV12 has 2 planes: Y plane (full resolution) and interleaved UV plane (half resolution)
#define NUM_PLANES  2
// YUYV has 1 plane with interleaved YUV data
// #define NUM_PLANES 1

typedef struct {
    void *start;
    size_t length;
} Plane;

typedef struct {
    Plane plane[VIDEO_MAX_PLANES];
} Buffer;


typedef struct {
    int fd;
    Buffer buffers[N_BUFFERS];
    guint n_buffers;

    GtkWidget *window;
    GtkWidget *drawing_area;
    GdkPixbuf *pixbuf;

    int cap_width;
    int cap_height;
    int disp_width;
    int disp_height;
} AppState;


void log_message (const char *fmt, ...);

#endif /* MAIN_H_ */
