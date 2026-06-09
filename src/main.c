/**
 * @file      main.c
 * @brief     Camera test main code
 * 
 * @date      2026-06-08
 * @author    Denys Stovbun
 */
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "app.h"

#define TIME_BUF_SIZE           31

#define DEFAULT_CAMERA          "/dev/video0"

static AppState *g_app = NULL;

/**
 * @brief Log messages global handler
 *
 * @param fmt message format
 * @param ... message arguments
 */
void log_message (const char *fmt, ...) {
    int r;
    char *msg = NULL;
    char tms[TIME_BUF_SIZE + 1];
    struct timeval tv;
    struct tm *ptm;

    // Format log message
    va_list arglist;
    va_start (arglist, fmt);
    r = vasprintf (&msg, fmt, arglist);
    va_end (arglist);

    // Format timestamp
    gettimeofday(&tv, NULL);
    ptm = localtime(&(tv.tv_sec));
    strftime(tms, TIME_BUF_SIZE, "%F %T", ptm);

    // Output log
    printf ("%s.%06lu: %s\n", tms, tv.tv_usec, msg);

    // Free formatted log buffer
    if (r && msg)
        free (msg);

    fflush(stdout);
}

static void on_signal(int signum) {
    (void)signum;
    if (g_app) {
        app_cleanup(g_app);
        g_app = NULL;
    }
    gtk_main_quit();
}

/**
 * @fn int main(int, char*[])
 * @brief Main function Screensaver application
 *
 * @param argc - number command line arguments
 * @param argv - command line arguments
 * @return application exit result code
 */
int main(int argc, char* argv[]) {
    log_message ("Starting version %s", APP_VERSION);

    char *device = DEFAULT_CAMERA;

    if (argc > 1 && strncmp(argv[1], "/dev/video", 10) == 0) {
        device = argv[1];
    }

    gtk_init (&argc, &argv);

    AppState app = {0};
    app.disp_width = IMAGE_WIDTH;
    app.disp_height = IMAGE_HEIGHT;

    log_message ("Use device %s [show as %dx%d]", device, app.disp_width, app.disp_height);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);

    if (app_init (&app, device) < 0) {
        log_message ("Failed to initialize camera");
        return 1;
    }

    app_ui (&app);

    gtk_main ();

    app_cleanup(&app);

    log_message ("Exiting");
    return 0;
}
