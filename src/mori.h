#ifndef MORI_H
#define MORI_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <lua.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MAX_WS 32
#define MAX_MON 16
#define MAX_BIND 256
#define MAX_RULE 128
#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    unsigned mods;
    KeySym key;
    char command[64], argument[512];
    int callback;
} Binding;

typedef struct {
    char class[128], instance[128], title[256];
    int workspace, floating, fullscreen;
} Rule;

typedef struct {
    char name[128];
    int workspace;
} MonitorRule;

typedef struct {
    lua_State *lua;
    int ref;
    int count, gap, border, masters;
    double ratio;
    bool sloppy;
    char names[MAX_WS][64], layouts[MAX_WS][16], normal[64], focus[64];
    Binding bindings[MAX_BIND];
    int nbindings;
    Rule rules[MAX_RULE];
    int nrules;
    MonitorRule monitors[MAX_MON];
    int nmonitors;
} Config;

typedef struct Client {
    Window win;
    int workspace, x, y, w, h, border, oldx, oldy, oldw, oldh;
    bool floating, fullscreen, mapped, dock;
    unsigned ignore_unmap;
    XSizeHints hints;
    char title[256], class[128], instance[128];
    struct Client *next;
} Client;

typedef struct {
    char name[128];
    int x, y, w, h, workspace;
} Monitor;

/* Shared window-manager state; X11 resources remain private to mori.c. */
extern Config config;
extern Monitor monitors[MAX_MON];
extern int nmonitors, selected_monitor;
extern Client *clients, *focused;

/* Window-manager operations used by Lua and IPC. */
const char *wm_command(const char *command, const char *argument);
void wm_spawn(const char *command);
bool wm_reload(void);
int workspace_monitor(int workspace);

/* Lua configuration and callbacks. */
bool lua_config_load(Config *config, const char *path, bool optional);
void lua_binding(const Binding *binding);
void lua_hook(const char *event, const Client *client);
void lua_startup(void);

static inline void copy(char *dst, size_t n, const char *src) {
    snprintf(dst, n, "%s", src ? src : "");
}

static inline int maximum(int a, int b) {
    return a > b ? a : b;
}

static inline int minimum(int a, int b) {
    return a < b ? a : b;
}

static inline bool valid_layout(const char *s) {
    return !strcmp(s, "tile") || !strcmp(s, "monocle") || !strcmp(s, "float");
}

#endif
