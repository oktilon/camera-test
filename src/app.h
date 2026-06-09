/**
 * @file   app.h
 * @author Denys Stovbun
 * @brief  Main application header
 * @date   2026-06-08
 * 
 */
#ifndef APP_H_
#define APP_H_
#include <gtk-3.0/gtk/gtk.h>
#include "main.h"

int app_init(AppState *app, const char *dev_name);
void app_ui(AppState *app);
void app_cleanup(AppState *app);

#endif /* APP_H_ */
