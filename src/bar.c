#include "protocol.h"
#include "bar.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <lauxlib.h>
#include <lualib.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void space(const char **p) {
    while (isspace((unsigned char)**p))
        ++*p;
}

static void string(lua_State *L, const char **p) {
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    ++*p;
    while (**p && **p != '"') {
        unsigned char c = (unsigned char)*(*p)++;
        if (c < 32)
            luaL_error(L, "control character in JSON string");
        if (c == '\\') {
            c = (unsigned char)*(*p)++;
            switch (c) {
            case '"':
            case '\\':
            case '/':
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'u': {
                unsigned v = 0;
                for (int i = 0; i < 4; ++i) {
                    unsigned char h = (unsigned char)**p;
                    if (!isxdigit(h))
                        luaL_error(L, "invalid JSON escape");
                    ++*p;
                    v = v * 16 + (isdigit(h) ? h - '0' : tolower(h) - 'a' + 10);
                }
                /* Mori emits ASCII escapes and literal UTF-8. */
                if (v > 127)
                    luaL_error(L, "unsupported JSON escape");
                c = (unsigned char)v;
                break;
            }
            default:
                luaL_error(L, "invalid JSON escape");
            }
        }
        luaL_addchar(&b, (char)c);
    }
    if (**p != '"')
        luaL_error(L, "unterminated JSON string");
    ++*p;
    luaL_pushresult(&b);
}

static void value(lua_State *L, const char **p, int depth) {
    if (depth > 16 || !lua_checkstack(L, 8))
        luaL_error(L, "JSON nesting too deep");
    space(p);
    if (**p == '"') {
        string(L, p);
        return;
    }
    if (**p == '{' || **p == '[') {
        int object = *(*p)++ == '{', end = object ? '}' : ']';
        lua_newtable(L);
        space(p);
        for (int i = 1; **p != end; ++i) {
            if (i > 1) {
                if (*(*p)++ != ',')
                    luaL_error(L, "expected JSON comma");
                space(p);
            }
            if (object) {
                if (**p != '"')
                    luaL_error(L, "expected JSON key");
                string(L, p);
                space(p);
                if (*(*p)++ != ':')
                    luaL_error(L, "expected JSON colon");
            }
            value(L, p, depth + 1);
            if (object)
                lua_rawset(L, -3);
            else
                lua_rawseti(L, -2, i);
            space(p);
        }
        ++*p;
        return;
    }
    if (!strncmp(*p, "true", 4)) {
        *p += 4;
        lua_pushboolean(L, 1);
    } else if (!strncmp(*p, "false", 5)) {
        *p += 5;
        lua_pushboolean(L, 0);
    } else if (!strncmp(*p, "null", 4)) {
        *p += 4;
        lua_pushnil(L);
    } else {
        const char *start = *p;
        if (**p == '-')
            ++*p;
        if (!isdigit((unsigned char)**p))
            luaL_error(L, "invalid JSON value");
        if (**p == '0')
            ++*p;
        else
            while (isdigit((unsigned char)**p))
                ++*p;
        if (**p == '.') {
            ++*p;
            if (!isdigit((unsigned char)**p))
                luaL_error(L, "invalid JSON number");
            while (isdigit((unsigned char)**p))
                ++*p;
        }
        if (**p == 'e' || **p == 'E') {
            ++*p;
            if (**p == '+' || **p == '-')
                ++*p;
            if (!isdigit((unsigned char)**p))
                luaL_error(L, "invalid JSON number");
            while (isdigit((unsigned char)**p))
                ++*p;
        }
        char *end;
        double n = strtod(start, &end);
        if (end != *p)
            luaL_error(L, "invalid JSON number");
        lua_pushnumber(L, n);
    }
}

static int decode(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    value(L, &p, 0);
    space(&p);
    if (*p)
        return luaL_error(L, "trailing JSON data");
    return 1;
}

int bar_json(lua_State *L, const char *text) {
    lua_pushcfunction(L, decode);
    lua_pushstring(L, text);
    return lua_pcall(L, 1, 1, 0) == LUA_OK;
}

typedef struct Icon {
    Window window;
    bool mapped;
    struct Icon *next;
} Icon;
static Display *dpy;
static int screen, icon_size, gap, padding, bar_width, bar_height;
static Window parent, owner;
static Atom selection, opcode, xembed, info;
static unsigned long background;
static Icon *icons;
static int (*previous_handler)(Display *, XErrorEvent *);

/* Foreign windows can disappear between any two requests. Limit error handling
 * to tray operations, leaving errors elsewhere visible. */
static int tray_error(Display *d, XErrorEvent *e) {
    if (e->error_code == BadWindow || e->error_code == BadMatch || e->error_code == BadDrawable)
        return 0;
    return previous_handler(d, e);
}
static void begin(void) {
    XSync(dpy, False);
    previous_handler = XSetErrorHandler(tray_error);
}
static void end(void) {
    XSync(dpy, False);
    XSetErrorHandler(previous_handler);
}
static void message(Window target, Atom type, long a, long b, long c, long d, long e) {
    XEvent event = {0};
    event.xclient = (XClientMessageEvent){.type = ClientMessage, .window = target,
        .message_type = type, .format = 32, .data.l = {a, b, c, d, e}};
    XSendEvent(dpy, target, False, type == xembed ? NoEventMask : StructureNotifyMask, &event);
}
static bool mapped(Window w) {
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char *data = NULL;
    bool result = true;
    if (XGetWindowProperty(dpy, w, info, 0, 2, False, info, &type, &format,
                           &count, &remaining, &data) == Success &&
        type == info && format == 32 && count == 2)
        result = (((unsigned long *)data)[1] & 1) != 0;
    if (data)
        XFree(data);
    return result;
}
void bar_tray_init(Display *display, int scr, Window bar) {
    dpy = display;
    screen = scr;
    parent = bar;
    char name[64];
    snprintf(name, sizeof name, "_NET_SYSTEM_TRAY_S%d", screen);
    selection = XInternAtom(dpy, name, False);
    opcode = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
    xembed = XInternAtom(dpy, "_XEMBED", False);
    info = XInternAtom(dpy, "_XEMBED_INFO", False);
}
void bar_tray_close(void) {
    if (!owner)
        return;
    begin();
    while (icons) {
        Icon *i = icons;
        icons = i->next;
        XUnmapWindow(dpy, i->window);
        XReparentWindow(dpy, i->window, RootWindow(dpy, screen), 0, 0);
        XRemoveFromSaveSet(dpy, i->window);
        XSelectInput(dpy, i->window, NoEventMask);
        free(i);
    }
    if (XGetSelectionOwner(dpy, selection) == owner)
        XSetSelectionOwner(dpy, selection, None, CurrentTime);
    XDestroyWindow(dpy, owner);
    owner = None;
    end();
}
void bar_tray_configure(bool enabled, int size, int spacing, int pad, unsigned long bg) {
    icon_size = size;
    gap = spacing;
    padding = pad;
    background = bg;
    if (!enabled) {
        bar_tray_close();
        return;
    }
    if (owner)
        return;
    /* Never displace another panel's tray. */
    XGrabServer(dpy);
    if (XGetSelectionOwner(dpy, selection) != None) {
        XUngrabServer(dpy);
        fprintf(stderr, "mori-bar: system tray is already owned by another panel\n");
        return;
    }
    XSetWindowAttributes attrs = {.override_redirect = True, .background_pixel = bg,
        .event_mask = SubstructureNotifyMask | SubstructureRedirectMask};
    owner = XCreateWindow(dpy, parent, 0, 0, 1, 1, 0, CopyFromParent, InputOutput,
                           CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);
    unsigned long orientation = 0, visual = XVisualIDFromVisual(DefaultVisual(dpy, screen));
    XChangeProperty(dpy, owner, XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False),
                    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&orientation, 1);
    XChangeProperty(dpy, owner, XInternAtom(dpy, "_NET_SYSTEM_TRAY_VISUAL", False),
                    XA_VISUALID, 32, PropModeReplace, (unsigned char *)&visual, 1);
    XSetSelectionOwner(dpy, selection, owner, CurrentTime);
    XUngrabServer(dpy);
    message(RootWindow(dpy, screen), XInternAtom(dpy, "MANAGER", False),
            CurrentTime, (long)selection, (long)owner, 0, 0);
}
int bar_tray_layout(int width, int height) {
    bar_width = width;
    bar_height = height;
    if (!owner)
        return 0;
    int size = icon_size < height ? icon_size : height;
    int count = 0;
    for (Icon *i = icons; i; i = i->next)
        count += i->mapped;
    if (!count) {
        begin();
        for (Icon *i = icons; i; i = i->next)
            XUnmapWindow(dpy, i->window);
        XUnmapWindow(dpy, owner);
        end();
        return 0;
    }
    int reserved = count * (size + gap) - gap + 2 * padding;
    if (reserved > width)
        reserved = width;
    begin();
    XSetWindowBackground(dpy, owner, background);
    XClearWindow(dpy, owner);
    XMoveResizeWindow(dpy, owner, width - reserved, 0, (unsigned)reserved, (unsigned)height);
    int x = padding;
    for (Icon *i = icons; i; i = i->next) {
        if (!i->mapped) {
            XUnmapWindow(dpy, i->window);
            continue;
        }
        XMoveResizeWindow(dpy, i->window, x, (height - size) / 2, (unsigned)size, (unsigned)size);
        XMapWindow(dpy, i->window);
        x += size + gap;
    }
    XMapRaised(dpy, owner);
    end();
    return reserved;
}
bool bar_tray_event(XEvent *e) {
    if (!owner)
        return false;
    if (e->type == SelectionClear && e->xselectionclear.selection == selection &&
        e->xselectionclear.window == owner) {
        bar_tray_close();
        return true;
    }
    if (e->type == ClientMessage && e->xclient.window == owner &&
        e->xclient.message_type == opcode && e->xclient.format == 32 && e->xclient.data.l[1] == 0) {
        Window w = (Window)e->xclient.data.l[2];
        int count = 0;
        for (Icon *i = icons; i; i = i->next) {
            if (i->window == w)
                return false;
            ++count;
        }
        if (!w || w == parent || w == owner || w == RootWindow(dpy, screen) || count >= 128)
            return false;
        begin();
        XWindowAttributes attrs;
        if (XGetWindowAttributes(dpy, w, &attrs) && attrs.class == InputOutput) {
            Icon *i = calloc(1, sizeof *i);
            if (i) {
                i->window = w;
                i->mapped = mapped(w);
                Icon **tail = &icons;
                while (*tail)
                    tail = &(*tail)->next;
                *tail = i;
                XSelectInput(dpy, w, StructureNotifyMask | PropertyChangeMask);
                XAddToSaveSet(dpy, w);
                XSetWindowBorderWidth(dpy, w, 0);
                XReparentWindow(dpy, w, owner, 0, 0);
                message(w, xembed, CurrentTime, 0, 0, (long)owner, 0);
                message(w, xembed, CurrentTime, 1, 0, 0, 0);
            }
        }
        end();
        return true;
    }
    Window w = e->xany.window;
    if (e->type == DestroyNotify) w = e->xdestroywindow.window;
    if (e->type == ReparentNotify) w = e->xreparent.window;
    if (e->type == ConfigureRequest) w = e->xconfigurerequest.window;
    if (e->type == MapRequest) w = e->xmaprequest.window;
    for (Icon **p = &icons; *p; p = &(*p)->next) {
        Icon *i = *p;
        if (i->window != w)
            continue;
        if (e->type == DestroyNotify || (e->type == ReparentNotify && e->xreparent.parent != owner)) {
            *p = i->next;
            if (e->type == ReparentNotify) {
                begin();
                XRemoveFromSaveSet(dpy, w);
                XSelectInput(dpy, w, NoEventMask);
                end();
            }
            free(i);
            return true;
        }
        if ((e->type == PropertyNotify && e->xproperty.atom == info) || e->type == MapRequest) {
            begin();
            i->mapped = mapped(w);
            end();
            return true;
        }
        if (e->type == ConfigureRequest) {
            bar_tray_layout(bar_width, bar_height);
            /* A denied resize still needs a reply with the assigned geometry. */
            begin();
            XWindowAttributes attrs;
            if (XGetWindowAttributes(dpy, w, &attrs)) {
                XEvent notify = {0};
                notify.xconfigure = (XConfigureEvent){.type = ConfigureNotify,
                    .event = w, .window = w, .x = attrs.x, .y = attrs.y,
                    .width = attrs.width, .height = attrs.height, .border_width = 0,
                    .above = None, .override_redirect = attrs.override_redirect};
                XSendEvent(dpy, w, False, StructureNotifyMask, &notify);
            }
            end();
            return true;
        }
        break;
    }
    return false;
}

#define CAP 65536

typedef struct {
    lua_State *lua;
    int height, padding;
    bool bottom, tray, show_title;
    int workspace_padding, workspace_spacing, tray_size, tray_spacing, tray_padding;
    char active_symbol[64], title_separator[128];
    char colors[4][64];
    XftColor palette[4];
    bool allocated[4];
    char font[256], foreground[64], background[64], monitor[128];
    XftFont *fonts;
    XftColor fg, bg;
    bool fg_allocated, bg_allocated;
} Settings;

typedef struct {
    int fd;
    char in[CAP], out[256];
    size_t used, sent, queued;
} Connection;

static Settings settings;
static Display *dpy;
static Window root, window;
static XftDraw *canvas;
static int screen, width, height, posx = -1, posy = -1;
static Atom partial, strut;
static volatile sig_atomic_t stopped, reload_requested;
static Connection subscription = {.fd = -1}, query = {.fd = -1};
static bool subscribed, dirty, pending, connected;
static int reply;
static long long retry_at, deadline;
static const char *queries[] = {"workspaces", "windows", "monitors"};
static char path[4096];

static long long milliseconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void signal_handler(int sig) {
    if (sig == SIGHUP)
        reload_requested = 1;
    else
        stopped = 1;
}

static void field(lua_State *L, const char *key, char *dst, size_t cap, const char *fallback) {
    lua_getfield(L, 1, key);
    const char *s = lua_isnil(L, -1) ? fallback : luaL_checkstring(L, -1);
    if (strlen(s) >= cap)
        luaL_error(L, "%s is too long", key);
    snprintf(dst, cap, "%s", s);
    lua_pop(L, 1);
}

static int integer(lua_State *L, const char *key, int fallback, int min, int max) {
    lua_getfield(L, 1, key);
    lua_Integer n = lua_isnil(L, -1) ? fallback : luaL_checkinteger(L, -1);
    if (n < min || n > max)
        luaL_error(L, "%s must be between %d and %d", key, min, max);
    lua_pop(L, 1);
    return (int)n;
}

static bool boolean(lua_State *L, const char *key, bool fallback) {
    lua_getfield(L, 1, key);
    bool value = fallback;
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TBOOLEAN);
        value = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

/* Workspace fg/bg, focused workspace fg, tray background. */
static const char *color_keys[] = {"workspace_foreground", "workspace_background",
    "workspace_active_foreground", "tray_background"};

static int parse_settings(lua_State *L) {
    Settings *s = lua_touserdata(L, lua_upvalueindex(1));
    luaL_checktype(L, 1, LUA_TTABLE);
    s->height = integer(L, "height", 26, 1, 256);
    s->padding = integer(L, "padding", 8, 0, 256);
    field(L, "font", s->font, sizeof s->font, "monospace");
    field(L, "foreground", s->foreground, sizeof s->foreground, "#eeeeee");
    field(L, "background", s->background, sizeof s->background, "#202020");
    field(L, "monitor", s->monitor, sizeof s->monitor, "");
    for (int i = 0; i < 4; ++i)
        field(L, color_keys[i], s->colors[i], sizeof s->colors[i],
              i == 0 || i == 2 ? s->foreground : s->background);
    s->tray = boolean(L, "tray", true);
    s->show_title = boolean(L, "show_title", true);
    s->workspace_padding = integer(L, "workspace_padding", 4, 0, 256);
    s->workspace_spacing = integer(L, "workspace_spacing", 0, 0, 256);
    s->tray_size = integer(L, "tray_icon_size", 20, 1, 256);
    s->tray_spacing = integer(L, "tray_spacing", 4, 0, 256);
    s->tray_padding = integer(L, "tray_padding", 8, 0, 256);
    field(L, "active_symbol", s->active_symbol, sizeof s->active_symbol, "●");
    field(L, "title_separator", s->title_separator, sizeof s->title_separator, " | ");
    char position[16];
    field(L, "position", position, sizeof position, "top");
    if (strcmp(position, "top") && strcmp(position, "bottom"))
        return luaL_error(L, "position must be top or bottom");
    s->bottom = !strcmp(position, "bottom");
    lua_getfield(L, 1, "format");
    if (!lua_isnil(L, -1))
        luaL_checktype(L, -1, LUA_TFUNCTION);
    lua_setglobal(L, "bar_format");
    return 0;
}

static void release(Settings *s) {
    if (s->fonts)
        XftFontClose(dpy, s->fonts);
    if (s->fg_allocated)
        XftColorFree(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &s->fg);
    if (s->bg_allocated)
        XftColorFree(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &s->bg);
    for (int i = 0; i < 4; ++i)
        if (s->allocated[i])
            XftColorFree(dpy, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen), &s->palette[i]);
    if (s->lua)
        lua_close(s->lua);
}

static bool load_settings(Settings *s, bool optional) {
    memset(s, 0, sizeof *s);
    lua_State *L = s->lua = luaL_newstate();
    if (!L)
        return false;
    luaL_openlibs(L);
    int rc;
    if (optional && access(path, F_OK) < 0 && errno == ENOENT) {
        lua_newtable(L);
        rc = LUA_OK;
    } else
        rc = luaL_loadfile(L, path) || lua_pcall(L, 0, 1, 0);
    if (!rc) {
        lua_pushlightuserdata(L, s);
        lua_pushcclosure(L, parse_settings, 1);
        lua_insert(L, -2);
        rc = lua_pcall(L, 1, 0, 0);
    }
    if (rc) {
        fprintf(stderr, "mori-bar: config: %s\n", lua_tostring(L, -1));
        release(s);
        return false;
    }
    lua_newtable(L);
    lua_setglobal(L, "snapshot");
    if (!dpy)
        return true;
    Visual *visual = DefaultVisual(dpy, screen);
    Colormap cmap = DefaultColormap(dpy, screen);
    s->fg_allocated = XftColorAllocName(dpy, visual, cmap, s->foreground, &s->fg);
    if (!s->fg_allocated)
        goto fail;
    s->bg_allocated = XftColorAllocName(dpy, visual, cmap, s->background, &s->bg);
    if (!s->bg_allocated)
        goto fail;
    for (int i = 0; i < 4; ++i) {
        s->allocated[i] = XftColorAllocName(dpy, visual, cmap, s->colors[i], &s->palette[i]);
        if (!s->allocated[i])
            goto fail;
    }
    s->fonts = XftFontOpenName(dpy, screen, s->font);
    if (s->fonts)
        return true;
fail:
    fprintf(stderr, "mori-bar: cannot load font or colors\n");
    release(s);
    return false;
}

static void append(char *dst, size_t cap, const char *s) {
    size_t used = strlen(dst);
    if (used < cap)
        snprintf(dst + used, cap - used, "%s", s);
}

static void default_text(lua_State *L, char *text, size_t cap) {
    if (!settings.show_title)
        return;
    lua_getglobal(L, "snapshot");
    char title[256] = "";
    lua_getfield(L, -1, "windows");
    if (lua_istable(L, -1))
        for (size_t i = 1; i <= lua_rawlen(L, -1); ++i) {
            lua_rawgeti(L, -1, (lua_Integer)i);
            lua_getfield(L, -1, "focused");
            bool active = lua_toboolean(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "title");
            const char *name = lua_tostring(L, -1);
            if (active)
                snprintf(title, sizeof title, "%s", name ? name : "");
            lua_pop(L, 2);
        }
    lua_pop(L, 2);
    append(text, cap, settings.title_separator);
    append(text, cap, title);
}

static int draw_workspaces(lua_State *L) {
    int x = settings.padding;
    XGlyphInfo circle;
    XftTextExtentsUtf8(dpy, settings.fonts, (const FcChar8 *)settings.active_symbol,
                      (int)strlen(settings.active_symbol), &circle);
    int slot = circle.width;
    lua_getglobal(L, "snapshot");
    lua_getfield(L, -1, "workspaces");
    if (lua_istable(L, -1)) {
        size_t count = lua_rawlen(L, -1);
        /* Measure every name, including the focused one, so focus cannot shift slots. */
        for (size_t i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, (lua_Integer)i);
            lua_getfield(L, -1, "name");
            const char *name = lua_tostring(L, -1);
            if (name) {
                XGlyphInfo extents;
                XftTextExtentsUtf8(dpy, settings.fonts, (const FcChar8 *)name,
                                  (int)strlen(name), &extents);
                if (extents.width > slot)
                    slot = extents.width;
            }
            lua_pop(L, 2);
        }
        slot += 2 * settings.workspace_padding;
        if (slot < 1)
            slot = 1;
        for (size_t i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, (lua_Integer)i);
            lua_getfield(L, -1, "focused");
            bool active = lua_toboolean(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "name");
            const char *name = active && settings.active_symbol[0] ? settings.active_symbol : lua_tostring(L, -1);
            XftDrawRect(canvas, &settings.palette[1], x, 0,
                        (unsigned)slot, (unsigned)height);
            if (name) {
                XGlyphInfo extents;
                XftTextExtentsUtf8(dpy, settings.fonts, (const FcChar8 *)name,
                                  (int)strlen(name), &extents);
                /* Center visible ink, accounting for glyph bearings and overhang. */
                XftDrawStringUtf8(canvas, &settings.palette[active ? 2 : 0], settings.fonts,
                                  x + (slot - extents.width) / 2 + extents.x,
                                  (height - extents.height) / 2 + extents.y,
                                  (const FcChar8 *)name, (int)strlen(name));
            }
            lua_pop(L, 2);
            x += slot + settings.workspace_spacing;
        }
    }
    lua_pop(L, 2);
    return x;
}

static void draw(void) {
    char text[8192] = "";
    bool workspaces = false;
    lua_State *L = settings.lua;
    int base = lua_gettop(L);
    if (!connected)
        snprintf(text, sizeof text, "mori-bar: waiting for Mori");
    else {
        lua_getglobal(L, "bar_format");
        bool custom = lua_isfunction(L, -1);
        if (custom) {
            lua_getglobal(L, "snapshot");
            if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
                fprintf(stderr, "mori-bar: format: %s\n", lua_tostring(L, -1));
                custom = false;
            } else if (lua_type(L, -1) != LUA_TSTRING) {
                fprintf(stderr, "mori-bar: format must return a string\n");
                custom = false;
            } else
                snprintf(text, sizeof text, "%s", lua_tostring(L, -1));
        }
        lua_settop(L, base);
        if (!custom) {
            default_text(L, text, sizeof text);
            workspaces = true;
        }
    }
    lua_settop(L, base);
    /* Keep output on one line; never interpret titles as markup or code. */
    for (char *p = text; *p; ++p)
        if ((unsigned char)*p < 32)
            *p = ' ';
    int reserved = bar_tray_layout(width, height);
    XftDrawSetClip(canvas, NULL);
    XftDrawRect(canvas, &settings.bg, 0, 0, (unsigned)width, (unsigned)height);
    int available = width - reserved - settings.padding;
    if (available < 0)
        available = 0;
    XRectangle clip = {0, 0, (unsigned short)available, (unsigned short)height};
    XftDrawSetClipRectangles(canvas, 0, 0, &clip, 1);
    int baseline = (height - settings.fonts->ascent - settings.fonts->descent) / 2 +
                   settings.fonts->ascent;
    int x = workspaces ? draw_workspaces(L) : settings.padding;
    XftDrawStringUtf8(canvas, &settings.fg, settings.fonts, x, baseline,
                      (const FcChar8 *)text, (int)strlen(text));
    XFlush(dpy);
}

static int number(lua_State *L, const char *name) {
    lua_getfield(L, -1, name);
    int n = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return n;
}

static void place(void) {
    XWindowAttributes a;
    XGetWindowAttributes(dpy, root, &a);
    int x = 0, y = 0, w = a.width, h = a.height;
    lua_State *L = settings.lua;
    lua_getglobal(L, "snapshot");
    lua_getfield(L, -1, "monitors");
    if (lua_istable(L, -1))
        for (size_t i = 1; i <= lua_rawlen(L, -1); ++i) {
            lua_rawgeti(L, -1, (lua_Integer)i);
            lua_getfield(L, -1, "name");
            const char *name = lua_tostring(L, -1);
            bool match = name && !strcmp(settings.monitor, name);
            lua_pop(L, 1);
            if (i == 1 || match) {
                x = number(L, "x");
                y = number(L, "y");
                w = number(L, "width");
                h = number(L, "height");
            }
            lua_pop(L, 1);
            if (match)
                break;
        }
    lua_pop(L, 2);
    if (w < 1 || h < 1)
        return;
    int barh = settings.height < h ? settings.height : h;
    if (settings.bottom)
        y += h - barh;
    if (x == posx && y == posy && width == w && height == barh)
        return;
    posx = x;
    posy = y;
    width = w;
    height = barh;
    unsigned long values[12] = {0};
    int edge = settings.bottom ? 3 : 2, range = settings.bottom ? 10 : 8;
    values[edge] = (unsigned long)(settings.bottom ? a.height - y : y + height);
    values[range] = (unsigned long)x;
    values[range + 1] = (unsigned long)(x + width - 1);
    XChangeProperty(dpy, window, partial, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)values,
                    12);
    XChangeProperty(dpy, window, strut, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)values,
                    4);
    XMoveResizeWindow(dpy, window, posx, posy, (unsigned)width, (unsigned)height);
}

static void disconnect(void) {
    if (subscription.fd >= 0)
        close(subscription.fd);
    if (query.fd >= 0)
        close(query.fd);
    subscription = (Connection){.fd = -1};
    query = (Connection){.fd = -1};
    subscribed = pending = connected = false;
    retry_at = milliseconds() + 1000;
    draw();
}

static bool open_connection(Connection *c) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (!socket_path(addr.sun_path, sizeof addr.sun_path))
        return false;
    c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c->fd < 0)
        return false;
    if (fcntl(c->fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(c->fd, F_SETFD, FD_CLOEXEC) < 0 ||
        connect(c->fd, (struct sockaddr *)&addr, sizeof addr) < 0)
        return false;
    return true;
}

static void send_request(Connection *c, const char *command) {
    c->sent = 0;
    c->queued = (size_t)snprintf(c->out, sizeof c->out, "{\"command\":\"%s\"}\n", command);
    deadline = milliseconds() + 5000;
}

static void refresh(void) {
    dirty = false;
    pending = true;
    reply = 0;
    send_request(&query, queries[reply]);
}

static bool receive_line(Connection *c, const char *line) {
    lua_State *L = settings.lua;
    int base = lua_gettop(L);
    if (!bar_json(L, line) || !lua_istable(L, -1)) {
        lua_settop(L, base);
        return false;
    }
    bool ok = true;
    if (c == &subscription && subscribed)
        dirty = true;
    else {
        lua_getfield(L, -1, "ok");
        ok = lua_isboolean(L, -1) && lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (ok && c == &subscription) {
            subscribed = true;
            dirty = true;
        } else if (ok && pending && reply < 3) {
            lua_getfield(L, -1, queries[reply]);
            ok = lua_istable(L, -1);
            if (ok) {
                lua_getglobal(L, "snapshot");
                lua_pushvalue(L, -2);
                lua_setfield(L, -2, queries[reply]);
                lua_pop(L, 1);
                if (++reply == 3) {
                    pending = false;
                    connected = true;
                    place();
                    draw();
                } else
                    send_request(&query, queries[reply]);
            }
        } else
            ok = false;
    }
    lua_settop(L, base);
    return ok;
}

static bool service(Connection *c, short events) {
    if (events & (POLLERR | POLLHUP | POLLNVAL))
        return false;
    if ((events & POLLOUT) && c->queued) {
        ssize_t n = write(c->fd, c->out + c->sent, c->queued - c->sent);
        if (n > 0) {
            c->sent += (size_t)n;
            if (c->sent == c->queued)
                c->sent = c->queued = 0;
        } else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            return false;
    }
    if (events & POLLIN) {
        ssize_t n = read(c->fd, c->in + c->used, sizeof c->in - c->used - 1);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return true;
        if (n <= 0)
            return false;
        c->used += (size_t)n;
        c->in[c->used] = 0;
        if (memchr(c->in, 0, c->used))
            return false;
        char *end;
        while ((end = memchr(c->in, '\n', c->used))) {
            *end = 0;
            if (!receive_line(c, c->in))
                return false;
            size_t consumed = (size_t)(end - c->in) + 1;
            memmove(c->in, c->in + consumed, c->used - consumed);
            c->used -= consumed;
            c->in[c->used] = 0;
        }
        if (c->used == sizeof c->in - 1)
            return false;
    }
    return true;
}

int main(int argc, char **argv) {
    bool check = argc == 3 && !strcmp(argv[1], "--check-config");
    bool explicit_path = argc == 3 && (!strcmp(argv[1], "--config") || check);
    if (argc != 1 && !explicit_path) {
        fprintf(stderr, "usage: mori-bar [--config PATH | --check-config PATH]\n");
        return 2;
    }
    const char *home = getenv("HOME");
    int len = explicit_path ? snprintf(path, sizeof path, "%s", argv[2])
              : home        ? snprintf(path, sizeof path, "%s/.config/mori/bar.lua", home)
                            : -1;
    if (len < 0 || len >= (int)sizeof path)
        return 1;
    setlocale(LC_CTYPE, "");
    if (!check) {
        dpy = XOpenDisplay(NULL);
        if (!dpy) {
            fprintf(stderr, "mori-bar: cannot open X display\n");
            return 1;
        }
        screen = DefaultScreen(dpy);
        root = RootWindow(dpy, screen);
    }
    if (!load_settings(&settings, !explicit_path)) {
        if (dpy)
            XCloseDisplay(dpy);
        return 1;
    }
    if (check) {
        release(&settings);
        puts("bar configuration valid");
        return 0;
    }
    window = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, settings.bg.pixel);
    canvas = XftDrawCreate(dpy, window, DefaultVisual(dpy, screen), DefaultColormap(dpy, screen));
    if (!canvas) {
        fprintf(stderr, "mori-bar: cannot create drawing context\n");
        XDestroyWindow(dpy, window);
        release(&settings);
        XCloseDisplay(dpy);
        return 1;
    }
    XSelectInput(dpy, window, ExposureMask | StructureNotifyMask);
    XSelectInput(dpy, root, StructureNotifyMask);
    Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    partial = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    strut = XInternAtom(dpy, "_NET_WM_STRUT", False);
    XChangeProperty(dpy, window, type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&dock, 1);
    XWMHints hints = {.flags = InputHint, .input = False};
    XSetWMHints(dpy, window, &hints);
    XClassHint class = {.res_name = "mori-bar", .res_class = "MoriBar"};
    XSetClassHint(dpy, window, &class);
    XStoreName(dpy, window, "Mori bar");
    bar_tray_init(dpy, screen, window);
    bar_tray_configure(settings.tray, settings.tray_size, settings.tray_spacing,
                       settings.tray_padding, settings.palette[3].pixel);
    place();
    XMapWindow(dpy, window);
    draw();
    struct sigaction sa = {.sa_handler = signal_handler};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    while (!stopped) {
        if (reload_requested) {
            reload_requested = 0;
            Settings next;
            if (load_settings(&next, false)) {
                release(&settings);
                settings = next;
                XSetWindowBackground(dpy, window, settings.bg.pixel);
                bar_tray_configure(settings.tray, settings.tray_size, settings.tray_spacing,
                                   settings.tray_padding, settings.palette[3].pixel);
                posx = -1;
                disconnect();
                place();
                draw();
            }
        }
        for (int budget = 256; budget && XPending(dpy); --budget) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (bar_tray_event(&e))
                draw();
            if (e.type == Expose && e.xexpose.window == window && !e.xexpose.count)
                draw();
            if (e.type == ConfigureNotify && e.xconfigure.window == root) {
                posx = -1;
                dirty = true;
                place();
                draw();
            }
        }
        long long now = milliseconds();
        if (subscription.fd < 0 && now >= retry_at) {
            if (open_connection(&subscription) && open_connection(&query))
                send_request(&subscription, "subscribe");
            else
                disconnect();
        }
        if (subscribed && !pending && dirty)
            refresh();
        if (subscription.fd >= 0 && (!subscribed || pending) && now >= deadline)
            disconnect();
        struct pollfd fds[] = {
            {.fd = ConnectionNumber(dpy), .events = POLLIN},
            {.fd = subscription.fd, .events = POLLIN | (subscription.queued ? POLLOUT : 0)},
            {.fd = query.fd, .events = POLLIN | (query.queued ? POLLOUT : 0)}};
        int rc = poll(fds, 3, XPending(dpy) ? 0 : 100);
        if (rc < 0 && errno != EINTR)
            break;
        if (rc <= 0)
            continue;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        /* Drain subscription first; events during a snapshot schedule another pass. */
        if (!service(&subscription, fds[1].revents) || !service(&query, fds[2].revents))
            disconnect();
    }
    if (subscription.fd >= 0)
        close(subscription.fd);
    if (query.fd >= 0)
        close(query.fd);
    bar_tray_close();
    XftDrawDestroy(canvas);
    XDestroyWindow(dpy, window);
    release(&settings);
    XCloseDisplay(dpy);
    return 0;
}
