#ifndef MORI_BAR_H
#define MORI_BAR_H
#include <lua.h>
#include <X11/Xlib.h>
#include <stdbool.h>

/* Push a decoded JSON value, or an error string. Never evaluates input as Lua. */
int bar_json(lua_State *L, const char *text);
void bar_tray_init(Display *display, int screen, Window parent);
void bar_tray_configure(bool enabled, int size, int spacing, int padding, unsigned long background);
int bar_tray_layout(int width, int height);
bool bar_tray_event(XEvent *event);
void bar_tray_close(void);
#endif
