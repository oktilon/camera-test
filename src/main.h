/**
 * @file    main.h
 * @brief   Main header
 * 
 * @fdate   2026-06-08
 * @author  Denys Stovbun
 */

#ifndef MAIN_H_
#define MAIN_H_

#include <wayland-client.h>
#include <stdint.h>
#include <unistd.h>

#include "config.h"

/** tunables *******************************/
#define CAPTURE_WIDTH   1280
#define CAPTURE_HEIGHT  720
#define DISP_WIDTH      640
#define DISP_HEIGHT     360
#define N_BUFFERS       4
#define NUM_PLANES      2       /* NV12 */

/** V4L2 buffer *****************************/
typedef struct { void *start; size_t length; } Plane;
typedef struct { Plane plane[NUM_PLANES]; } CamBuffer;

/** Wayland state ****************************/
typedef struct {
    struct wl_display    *display;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    struct wl_shm        *shm;

    struct wl_surface      *surface;
    struct wl_shell_surface *shell_surface;
    struct wl_shm_pool     *pool;
    struct wl_buffer       *buffer;

    struct xdg_wm_base     *xdg_wm_base;
    struct xdg_surface     *xdg_surface;
    struct xdg_toplevel    *xdg_toplevel;
    int      configured;    /* wait for configure before committing */

    uint8_t *shm_data;      /* mmap'd shared memory we write RGB here    */
    int      shm_fd;
    int      shm_size;      /* DISP_WIDTH * DISP_HEIGHT * 4 (XRGB8888)     */
} WlState;

/** App state ******************************/
typedef struct {
    int        cam_fd;
    CamBuffer  buffers[N_BUFFERS];
    int        n_buffers;
    int        cap_w, cap_h;
    int        disp_w, disp_h;
    WlState    wl;
    volatile int running;
} AppState;


#endif /* MAIN_H_ */
