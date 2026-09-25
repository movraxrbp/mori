#include "mori.h"
#include "ipc.h"
#include "geometry.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

static Display *dpy;
static Window root, support;
static int screen, randr_event, randr_error;
int nmonitors, selected_monitor;
static bool randr, running = true, reload_pending;
static volatile sig_atomic_t stopped;
Config config;
Monitor monitors[MAX_MON];
Client *clients, *focused;
static unsigned long normal_pixel, focus_pixel;
static unsigned numlock_mask;
static char config_path[4096];
static Atom wm_protocols, wm_delete, wm_take_focus, wm_state, utf8;
static Atom net_supported, net_support, net_name, net_active, net_clients;
static Atom net_desktops, net_current, net_names, net_wm_desktop, net_state, net_fullscreen;
static Atom net_type, net_dialog, net_dock;
static Atom net_strut, net_strut_partial, net_workarea;
static void arrange(void);
static void emit(const char *, Client *);
static void focus(Client *);
static void grabkeys(void);
static void update_desktops(void);

static void emit(const char *event, Client *c) {
    ipc_emit(event, c);
    lua_hook(event, c);
}

void wm_spawn(const char *cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        if (dpy)
            close(ConnectionNumber(dpy));
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid < 0)
        perror("mori: fork");
}

static Client *find(Window w) {
    for (Client *c = clients; c; c = c->next)
        if (c->win == w)
            return c;
    return NULL;
}

int workspace_monitor(int ws) {
    for (int i = 0; i < nmonitors; ++i)
        if (monitors[i].workspace == ws)
            return i;
    return -1;
}

static bool visible(Client *c) {
    return c->dock || workspace_monitor(c->workspace) >= 0;
}

static void property(Window win, Atom atom, unsigned long value) {
    XChangeProperty(dpy, win, atom, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&value, 1);
}

static bool has_atom(Window w, Atom prop, Atom needle) {
    Atom type;
    int format;
    unsigned long n, left;
    unsigned char *data = NULL;
    bool found = false;
    if (XGetWindowProperty(dpy, w, prop, 0, 1024, False, XA_ATOM, &type, &format, &n, &left,
                           &data) == Success &&
        type == XA_ATOM && format == 32)
        for (unsigned long i = 0; i < n; ++i)
            if (((Atom *)data)[i] == needle)
                found = true;
    if (data)
        XFree(data);
    return found;
}

static void client_list(void) {
    size_t n = 0;
    for (Client *c = clients; c; c = c->next)
        ++n;
    Window *wins = calloc(n ? n : 1, sizeof *wins);
    if (!wins)
        return;
    size_t i = 0;
    for (Client *c = clients; c; c = c->next)
        wins[i++] = c->win;
    XChangeProperty(dpy, root, net_clients, XA_WINDOW, 32, PropModeReplace, (unsigned char *)wins,
                    (int)n);
    free(wins);
}

static void title(Client *c) {
    Atom type;
    int format;
    unsigned long n, left;
    unsigned char *data = NULL;
    c->title[0] = 0;
    if (XGetWindowProperty(dpy, c->win, net_name, 0, 64, False, utf8, &type, &format, &n, &left,
                           &data) == Success &&
        type == utf8 && format == 8 && data)
        snprintf(c->title, sizeof c->title, "%.*s", (int)n, data);
    if (data)
        XFree(data);
    if (!c->title[0]) {
        char *name = NULL;
        if (XFetchName(dpy, c->win, &name) && name) {
            copy(c->title, sizeof c->title, name);
            XFree(name);
        }
    }
}

static void notify_configure(Client *c) {
    XEvent e = {0};
    e.xconfigure.type = ConfigureNotify;
    e.xconfigure.display = dpy;
    e.xconfigure.event = e.xconfigure.window = c->win;
    e.xconfigure.x = c->x;
    e.xconfigure.y = c->y;
    e.xconfigure.width = c->w;
    e.xconfigure.height = c->h;
    e.xconfigure.border_width = c->border;
    XSendEvent(dpy, c->win, False, StructureNotifyMask, &e);
}

static void workarea(Monitor *, int *, int *, int *, int *);

static void read_hints(Client *c) {
    long supplied;
    memset(&c->hints, 0, sizeof c->hints);
    if (!XGetWMNormalHints(dpy, c->win, &c->hints, &supplied))
        c->hints.flags = 0;
}

static void resize(Client *c, int x, int y, int w, int h) {
    if (!c->fullscreen && !c->dock) {
        int mi = workspace_monitor(c->workspace);
        if (mi < 0)
            mi = selected_monitor;
        int wx, wy, ww, wh;
        workarea(&monitors[mi], &wx, &wy, &ww, &wh);
        int border = minimum(config.border, (minimum(ww, wh) - 1) / 2);
        bool floating = c->floating || !strcmp(config.layouts[c->workspace], "float");
        int maxw = maximum(1, ww - 2 * border), maxh = maximum(1, wh - 2 * border);
        if (!floating) {
            maxw = minimum(maxw, maximum(1, w));
            maxh = minimum(maxh, maximum(1, h));
        }
        size_hints(&c->hints, maxw, maxh, &w, &h);
        x = maximum(wx, minimum(x, wx + ww - w - 2 * border));
        y = maximum(wy, minimum(y, wy + wh - h - 2 * border));
        c->border = border;
    } else
        c->border = 0;
    XSetWindowBorderWidth(dpy, c->win, (unsigned)c->border);
    c->x = x;
    c->y = y;
    c->w = maximum(1, w);
    c->h = maximum(1, h);
    XMoveResizeWindow(dpy, c->win, c->x, c->y, (unsigned)c->w, (unsigned)c->h);
    notify_configure(c);
}

static void workarea(Monitor *m, int *x, int *y, int *w, int *h) {
    XWindowAttributes rootattr;
    XGetWindowAttributes(dpy, root, &rootattr);
    int rootw = rootattr.width, rooth = rootattr.height;
    Geometry monitor = {m->x, m->y, m->w, m->h}, area = monitor;
    for (Client *c = clients; c; c = c->next) {
        if (!c->dock)
            continue;
        Atom actual;
        int format;
        unsigned long n, left;
        unsigned char *data = NULL;
        unsigned long vals[12] = {0};
        if (XGetWindowProperty(dpy, c->win, net_strut_partial, 0, 12, False, XA_CARDINAL, &actual,
                               &format, &n, &left, &data) == Success &&
            actual == XA_CARDINAL && format == 32 && n >= 12) {
            for (int i = 0; i < 12; ++i)
                vals[i] = ((unsigned long *)data)[i];
        } else {
            if (data)
                XFree(data);
            data = NULL;
            if (XGetWindowProperty(dpy, c->win, net_strut, 0, 4, False, XA_CARDINAL, &actual,
                                   &format, &n, &left, &data) != Success ||
                actual != XA_CARDINAL || format != 32 || n < 4) {
                if (data)
                    XFree(data);
                continue;
            }
            for (int i = 0; i < 4; ++i)
                vals[i] = ((unsigned long *)data)[i];
            vals[4] = vals[6] = 0;
            vals[5] = vals[7] = (unsigned long)(rooth - 1);
            vals[8] = vals[10] = 0;
            vals[9] = vals[11] = (unsigned long)(rootw - 1);
        }
        if (data)
            XFree(data);
        reserve_strut(monitor, rootw, rooth, vals, &area);
    }
    *x = area.x;
    *y = area.y;
    *w = area.w;
    *h = area.h;
}

static void arrange(void) {
    for (Client *c = clients; c; c = c->next) {
        if (visible(c) && !c->mapped) {
            XMapWindow(dpy, c->win);
            c->mapped = true;
        }
        if (!visible(c) && c->mapped) {
            ++c->ignore_unmap;
            XUnmapWindow(dpy, c->win);
            c->mapped = false;
        }
    }
    for (int mi = 0; mi < nmonitors; ++mi) {
        Monitor *m = &monitors[mi];
        if (m->workspace < 0)
            continue;
        const char *layout = config.layouts[m->workspace];
        int wx, wy, ww, wh;
        workarea(m, &wx, &wy, &ww, &wh);
        int n = 0;
        for (Client *c = clients; c; c = c->next)
            if (c->workspace == m->workspace && !c->floating && !c->fullscreen)
                ++n;
        int masters = minimum(n, config.masters), stack = n - masters, index = 0;
        int g = minimum(config.gap, minimum(ww, wh) / 4), b = config.border;
        int area_w = maximum(1, ww - 2 * g), area_h = maximum(1, wh - 2 * g);
        int master_w = stack ? (int)((area_w - g) * config.ratio) : area_w;
        for (Client *c = clients; c; c = c->next) {
            if (c->workspace != m->workspace || c->dock)
                continue;
            if (c->fullscreen) {
                resize(c, m->x, m->y, m->w, m->h);
                XRaiseWindow(dpy, c->win);
                continue;
            }
            if (c->floating || !strcmp(layout, "float")) {
                resize(c, maximum(wx, minimum(c->x, wx + ww - c->w - 2 * b)),
                       maximum(wy, minimum(c->y, wy + wh - c->h - 2 * b)),
                       minimum(c->w, ww - 2 * b), minimum(c->h, wh - 2 * b));
                continue;
            }
            if (!strcmp(layout, "monocle"))
                resize(c, wx + g, wy + g, area_w - 2 * b, area_h - 2 * b);
            else {
                bool master = index < masters;
                int count = master ? masters : stack, row = master ? index : index - masters;
                int available = maximum(count, area_h - (count - 1) * g);
                int top = row * available / count, bottom = (row + 1) * available / count;
                resize(c, wx + g + (master ? 0 : master_w + g), wy + g + top + row * g,
                       (master ? master_w : area_w - master_w - g) - 2 * b, bottom - top - 2 * b);
            }
            ++index;
        }
        /* Fullscreen always stays above tiled and floating clients. */
        for (Client *c = clients; c; c = c->next)
            if (c->workspace == m->workspace && c->fullscreen)
                XRaiseWindow(dpy, c->win);
    }
    update_desktops();
    XFlush(dpy);
}

static void send_protocol(Client *c, Atom protocol) {
    XEvent e = {0};
    e.xclient.type = ClientMessage;
    e.xclient.window = c->win;
    e.xclient.message_type = wm_protocols;
    e.xclient.format = 32;
    e.xclient.data.l[0] = (long)protocol;
    e.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, c->win, False, NoEventMask, &e);
}

static bool supports(Client *c, Atom protocol) {
    Atom *list = NULL;
    int n = 0;
    bool yes = false;
    if (XGetWMProtocols(dpy, c->win, &list, &n)) {
        for (int i = 0; i < n; ++i)
            if (list[i] == protocol)
                yes = true;
        XFree(list);
    }
    return yes;
}

static void focus(Client *c) {
    if (c && (!visible(c) || c->dock))
        c = NULL;
    if (!c)
        for (Client *p = clients; p; p = p->next)
            if (p->workspace == monitors[selected_monitor].workspace && !p->dock) {
                c = p;
                break;
            }
    if (focused)
        XSetWindowBorder(dpy, focused->win, normal_pixel);
    bool changed = focused != c;
    focused = c;
    if (c) {
        selected_monitor = workspace_monitor(c->workspace);
        XSetWindowBorder(dpy, c->win, focus_pixel);
        XWMHints *h = XGetWMHints(dpy, c->win);
        if (!h || !(h->flags & InputHint) || h->input)
            XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
        if (h)
            XFree(h);
        if (supports(c, wm_take_focus))
            send_protocol(c, wm_take_focus);
        if (c->floating || c->fullscreen || strcmp(config.layouts[c->workspace], "tile"))
            XRaiseWindow(dpy, c->win);
        XChangeProperty(dpy, root, net_active, XA_WINDOW, 32, PropModeReplace,
                        (unsigned char *)&c->win, 1);
    } else {
        XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
        XDeleteProperty(dpy, root, net_active);
    }
    update_desktops();
    if (changed)
        emit("focus", c);
}

static void fullscreen(Client *c, bool enabled) {
    if (!c || c->dock || c->fullscreen == enabled)
        return;
    c->fullscreen = enabled;
    if (enabled) {
        c->oldx = c->x;
        c->oldy = c->y;
        c->oldw = c->w;
        c->oldh = c->h;
    } else
        resize(c, c->oldx, c->oldy, c->oldw, c->oldh);
    /* Preserve state atoms owned by applications. */
    Atom type;
    int fmt;
    unsigned long n = 0, left;
    unsigned char *data = NULL;
    XGetWindowProperty(dpy, c->win, net_state, 0, 1024, False, XA_ATOM, &type, &fmt, &n, &left,
                       &data);
    if (type != XA_ATOM || fmt != 32)
        n = 0;
    Atom states[1025];
    int count = 0;
    for (unsigned long i = 0; i < n; ++i)
        if (((Atom *)data)[i] != net_fullscreen)
            states[count++] = ((Atom *)data)[i];
    if (enabled)
        states[count++] = net_fullscreen;
    XChangeProperty(dpy, c->win, net_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)states,
                    count);
    if (data)
        XFree(data);
    arrange();
    emit("fullscreen", c);
}

static void manage(Window w) {
    XWindowAttributes a;
    if (find(w) || !XGetWindowAttributes(dpy, w, &a) || a.override_redirect || a.class == InputOnly)
        return;
    Client *c = calloc(1, sizeof *c);
    if (!c)
        return;
    c->win = w;
    c->workspace = monitors[selected_monitor].workspace;
    if (c->workspace < 0)
        c->workspace = 0;
    c->x = a.x;
    c->y = a.y;
    c->w = a.width;
    c->h = a.height;
    c->mapped = a.map_state != IsUnmapped;
    title(c);
    XClassHint hint = {0};
    if (XGetClassHint(dpy, w, &hint)) {
        copy(c->class, sizeof c->class, hint.res_class);
        copy(c->instance, sizeof c->instance, hint.res_name);
        if (hint.res_class)
            XFree(hint.res_class);
        if (hint.res_name)
            XFree(hint.res_name);
    }
    Window transient;
    Client *parent = NULL;
    if (XGetTransientForHint(dpy, w, &transient)) {
        c->floating = true;
        parent = find(transient);
        if (parent)
            c->workspace = parent->workspace;
    }
    c->dock = has_atom(w, net_type, net_dock);
    if (c->dock || has_atom(w, net_type, net_dialog))
        c->floating = true;
    read_hints(c);
    if ((c->hints.flags & PMinSize) && (c->hints.flags & PMaxSize) &&
        c->hints.min_width == c->hints.max_width && c->hints.min_height == c->hints.max_height)
        c->floating = true;
    bool full = has_atom(w, net_state, net_fullscreen);
    for (int i = 0; i < config.nrules; ++i) {
        Rule *r = &config.rules[i];
        if ((r->class[0] && strcmp(r->class, c->class)) ||
            (r->instance[0] && strcmp(r->instance, c->instance)) ||
            (r->title[0] && !strstr(c->title, r->title)))
            continue;
        if (r->workspace >= 0)
            c->workspace = r->workspace;
        if (r->floating >= 0)
            c->floating = r->floating;
        if (r->fullscreen >= 0)
            full = r->fullscreen;
    }
    if (c->dock)
        c->floating = true;
    int mi = workspace_monitor(c->workspace);
    if (mi < 0)
        mi = selected_monitor;
    if (c->floating && !c->dock) {
        c->x = monitors[mi].x + (monitors[mi].w - c->w) / 2;
        c->y = monitors[mi].y + (monitors[mi].h - c->h) / 2;
    }
    c->next = clients;
    clients = c;
    XSelectInput(dpy, w, PropertyChangeMask | EnterWindowMask | FocusChangeMask);
    if (c->dock) {
        XSetWindowBorderWidth(dpy, w, 0);
    } else {
        XSetWindowBorder(dpy, w, normal_pixel);
        XSetWindowBorderWidth(dpy, w, (unsigned)config.border);
    }
    XAddToSaveSet(dpy, w);
    if (!c->dock) {
        XGrabButton(dpy, Button1, AnyModifier, w, False, ButtonPressMask, GrabModeSync,
                    GrabModeAsync, None, None);
        XGrabButton(dpy, Button3, Mod4Mask, w, False, ButtonPressMask, GrabModeSync, GrabModeAsync,
                    None, None);
    }
    long state[2] = {NormalState, None};
    XChangeProperty(dpy, w, wm_state, wm_state, 32, PropModeReplace, (unsigned char *)state, 2);
    property(w, net_wm_desktop, c->dock ? 0xffffffffUL : (unsigned long)c->workspace);
    client_list();
    arrange();
    if (full && !c->dock)
        fullscreen(c, true);
    /* Hooks may close/move clients; do not dereference c after emitting. */
    if (visible(c) && !c->dock)
        focus(c);
    emit("window_open", c);
}

static void unmanage(Client *c, bool destroyed) {
    bool was_focused = focused == c;
    Client **link = &clients;
    while (*link && *link != c)
        link = &(*link)->next;
    if (*link)
        *link = c->next;
    if (was_focused)
        focused = NULL;
    if (!destroyed) {
        XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
        XRemoveFromSaveSet(dpy, c->win);
        XSetWindowBorderWidth(dpy, c->win, 0);
        XDeleteProperty(dpy, c->win, wm_state);
    }
    client_list();
    arrange();
    emit("window_close", c);
    free(c);
    if (was_focused)
        focus(NULL);
}

static void update_window_type(Client *c) {
    bool dock = has_atom(c->win, net_type, net_dock);
    if (dock == c->dock)
        return;
    if (dock && c->fullscreen)
        fullscreen(c, false);
    c->dock = dock;
    property(c->win, net_wm_desktop, dock ? 0xffffffffUL : (unsigned long)c->workspace);
    if (dock) {
        c->floating = true;
        XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
        c->border = 0;
        XSetWindowBorderWidth(dpy, c->win, 0);
    } else {
        XSetWindowBorder(dpy, c->win, c == focused ? focus_pixel : normal_pixel);
        XSetWindowBorderWidth(dpy, c->win, (unsigned)config.border);
        XGrabButton(dpy, Button1, AnyModifier, c->win, False, ButtonPressMask, GrabModeSync,
                    GrabModeAsync, None, None);
        XGrabButton(dpy, Button3, Mod4Mask, c->win, False, ButtonPressMask, GrabModeSync,
                    GrabModeAsync, None, None);
    }
    arrange();
    if (dock && focused == c)
        focus(NULL);
    emit("workarea", c);
}

static void update_desktops(void) {
    XWindowAttributes a;
    XGetWindowAttributes(dpy, root, &a);
    Monitor desktop = {.w = a.width, .h = a.height};
    int x, y, w, h;
    workarea(&desktop, &x, &y, &w, &h);
    unsigned long areas[MAX_WS * 4];
    for (int i = 0; i < config.count; ++i) {
        areas[4 * i] = (unsigned long)x;
        areas[4 * i + 1] = (unsigned long)y;
        areas[4 * i + 2] = (unsigned long)w;
        areas[4 * i + 3] = (unsigned long)h;
    }
    XChangeProperty(dpy, root, net_workarea, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)areas, config.count * 4);
    property(root, net_desktops, (unsigned long)config.count);
    int ws = monitors[selected_monitor].workspace;
    property(root, net_current, (unsigned long)maximum(0, ws));
    char names[MAX_WS * 64];
    size_t used = 0;
    for (int i = 0; i < config.count; ++i) {
        size_t len = strlen(config.names[i]) + 1;
        memcpy(names + used, config.names[i], len);
        used += len;
    }
    XChangeProperty(dpy, root, net_names, utf8, 8, PropModeReplace, (unsigned char *)names,
                    (int)used);
}

static void discover_monitors(bool preferences) {
    Monitor old[MAX_MON];
    int old_count = nmonitors;
    memcpy(old, monitors, sizeof old);
    nmonitors = 0;
    if (randr) {
        XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
        if (res) {
            for (int i = 0; i < res->noutput && nmonitors < MAX_MON; ++i) {
                XRROutputInfo *out = XRRGetOutputInfo(dpy, res, res->outputs[i]);
                if (!out)
                    continue;
                if (out->connection == RR_Connected && out->crtc) {
                    XRRCrtcInfo *crtc = XRRGetCrtcInfo(dpy, res, out->crtc);
                    if (crtc && crtc->width && crtc->height) {
                        bool duplicate = false;
                        for (int j = 0; j < nmonitors; ++j)
                            if (monitors[j].x == crtc->x && monitors[j].y == crtc->y)
                                duplicate = true;
                        if (!duplicate) {
                            Monitor *m = &monitors[nmonitors++];
                            memset(m, 0, sizeof *m);
                            snprintf(m->name, sizeof m->name, "%.*s", out->nameLen, out->name);
                            m->x = crtc->x;
                            m->y = crtc->y;
                            m->w = (int)crtc->width;
                            m->h = (int)crtc->height;
                        }
                    }
                    if (crtc)
                        XRRFreeCrtcInfo(crtc);
                }
                XRRFreeOutputInfo(out);
            }
            XRRFreeScreenResources(res);
        }
    }
    if (!nmonitors) {
        XWindowAttributes a;
        XGetWindowAttributes(dpy, root, &a);
        monitors[0] = (Monitor){.w = a.width, .h = a.height};
        copy(monitors[0].name, sizeof monitors[0].name, "default");
        nmonitors = 1;
    }
    bool used[MAX_WS] = {0};
    for (int i = 0; i < nmonitors; ++i) {
        Monitor *m = &monitors[i];
        int ws = -1;
        for (int j = 0; j < old_count; ++j)
            if (!strcmp(old[j].name, m->name))
                ws = old[j].workspace;
        if (preferences)
            for (int j = 0; j < config.nmonitors; ++j)
                if (!strcmp(config.monitors[j].name, m->name))
                    ws = config.monitors[j].workspace;
        if (ws < 0 || ws >= config.count || used[ws]) {
            ws = 0;
            while (ws < config.count && used[ws])
                ++ws;
        }
        if (ws == config.count)
            ws = -1; /* More outputs than workspaces: remaining outputs are empty. */
        m->workspace = ws;
        if (ws >= 0)
            used[ws] = true;
    }
    selected_monitor = minimum(selected_monitor, nmonitors - 1);
    arrange();
    focus(focused && visible(focused) ? focused : NULL);
    update_desktops();
    emit("monitor", NULL);
}

static void grabkeys(void) {
    numlock_mask = 0;
    XModifierKeymap *map = XGetModifierMapping(dpy);
    KeyCode num = XKeysymToKeycode(dpy, XK_Num_Lock);
    if (map) {
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < map->max_keypermod; ++j)
                if (map->modifiermap[i * map->max_keypermod + j] == num)
                    numlock_mask = 1U << i;
        XFreeModifiermap(map);
    }
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    unsigned locks[] = {0, LockMask, numlock_mask, numlock_mask | LockMask};
    for (int i = 0; i < config.nbindings; ++i) {
        Binding *b = &config.bindings[i];
        KeyCode key = XKeysymToKeycode(dpy, b->key);
        if (key)
            for (size_t j = 0; j < LENGTH(locks); ++j)
                XGrabKey(dpy, key, b->mods | locks[j], root, True, GrabModeAsync, GrabModeAsync);
    }
}

static bool colors(Config *c, unsigned long *normal, unsigned long *active) {
    XColor exact, color;
    if (!XAllocNamedColor(dpy, DefaultColormap(dpy, screen), c->normal, &color, &exact))
        return false;
    *normal = color.pixel;
    if (!XAllocNamedColor(dpy, DefaultColormap(dpy, screen), c->focus, &color, &exact)) {
        XFreeColors(dpy, DefaultColormap(dpy, screen), normal, 1, 0);
        return false;
    }
    *active = color.pixel;
    return true;
}

bool wm_reload(void) {
    reload_pending = false;
    Config next;
    unsigned long normal, active;
    if (!lua_config_load(&next, config_path, false))
        return false;
    if (!colors(&next, &normal, &active)) {
        fprintf(stderr, "mori: invalid border color\n");
        lua_close(next.lua);
        return false;
    }
    lua_close(config.lua);
    XFreeColors(dpy, DefaultColormap(dpy, screen), &normal_pixel, 1, 0);
    XFreeColors(dpy, DefaultColormap(dpy, screen), &focus_pixel, 1, 0);
    config = next;
    normal_pixel = normal;
    focus_pixel = active;
    for (Client *c = clients; c; c = c->next) {
        c->workspace = minimum(c->workspace, config.count - 1);
        property(c->win, net_wm_desktop, c->dock ? 0xffffffffUL : (unsigned long)c->workspace);
        XSetWindowBorder(dpy, c->win, c == focused ? focus_pixel : normal_pixel);
    }
    grabkeys();
    discover_monitors(true);
    emit("reload", NULL);
    return true;
}

static int number(const char *arg, int hi) {
    char *end;
    errno = 0;
    long n = strtol(arg, &end, 10);
    return !errno && *arg && !*end && n >= 1 && n <= hi ? (int)n - 1 : -1;
}

const char *wm_command(const char *cmd, const char *arg) {
    if (!strcmp(cmd, "workspace")) {
        int ws = number(arg, config.count);
        if (ws < 0)
            return "invalid workspace";
        int mi = workspace_monitor(ws);
        if (mi >= 0)
            selected_monitor = mi;
        else
            monitors[selected_monitor].workspace = ws;
        arrange();
        focus(NULL);
        emit("workspace", focused);
    } else if (!strcmp(cmd, "move-workspace")) {
        int ws = number(arg, config.count);
        if (ws < 0)
            return "invalid workspace";
        if (!focused)
            return "no focused window";
        Client *c = focused;
        c->workspace = ws;
        property(c->win, net_wm_desktop, (unsigned long)ws);
        arrange();
        focus(NULL);
        emit("workspace", c);
    } else if (!strcmp(cmd, "focus")) {
        if (strcmp(arg, "next") && strcmp(arg, "prev"))
            return "focus expects next or prev";
        Client *first = NULL, *last = NULL, *previous = NULL, *target = NULL;
        int ws = monitors[selected_monitor].workspace;
        for (Client *c = clients; c; c = c->next)
            if (c->workspace == ws && !c->dock) {
                if (!first)
                    first = c;
                if (previous == focused && focused && !strcmp(arg, "next"))
                    target = c;
                if (c == focused && !strcmp(arg, "prev"))
                    target = previous;
                previous = last = c;
            }
        focus(target ? target : (!strcmp(arg, "prev") ? last : first));
    } else if (!strcmp(cmd, "monitor")) {
        int mi;
        if (!strcmp(arg, "next"))
            mi = (selected_monitor + 1) % nmonitors;
        else if (!strcmp(arg, "prev"))
            mi = (selected_monitor + nmonitors - 1) % nmonitors;
        else {
            mi = number(arg, nmonitors);
            if (mi < 0)
                return "invalid monitor";
        }
        selected_monitor = mi;
        focus(NULL);
        emit("workspace", focused);
    } else if (!strcmp(cmd, "close")) {
        if (!focused)
            return "no focused window";
        if (supports(focused, wm_delete))
            send_protocol(focused, wm_delete);
        else
            XKillClient(dpy, focused->win);
    } else if (!strcmp(cmd, "layout")) {
        if (!valid_layout(arg))
            return "layout expects tile, monocle or float";
        int ws = monitors[selected_monitor].workspace;
        if (ws < 0)
            return "monitor has no workspace";
        copy(config.layouts[ws], sizeof config.layouts[ws], arg);
        arrange();
        emit("layout", focused);
    } else if (!strcmp(cmd, "floating") || !strcmp(cmd, "fullscreen")) {
        if (!focused)
            return "no focused window";
        if (*arg && strcmp(arg, "toggle") && strcmp(arg, "on") && strcmp(arg, "off"))
            return "expected on, off or toggle";
        bool current = !strcmp(cmd, "floating") ? focused->floating : focused->fullscreen;
        bool value = !strcmp(arg, "on") || ((!*arg || !strcmp(arg, "toggle")) && !current);
        if (!strcmp(cmd, "fullscreen"))
            fullscreen(focused, value);
        else {
            focused->floating = value;
            arrange();
            emit("floating", focused);
        }
    } else if (!strcmp(cmd, "master-ratio")) {
        char *end;
        double ratio = strtod(arg, &end);
        if (!*arg || *end || !(ratio >= .1 && ratio <= .9))
            return "ratio must be 0.1..0.9";
        config.ratio = ratio;
        arrange();
        emit("layout", focused);
    } else if (!strcmp(cmd, "spawn")) {
        if (!*arg)
            return "spawn needs a shell command";
        wm_spawn(arg);
    } else if (!strcmp(cmd, "reload"))
        reload_pending = true;
    else if (!strcmp(cmd, "quit"))
        running = false;
    else
        return "unknown command";
    return NULL;
}

static Window drag_window;
static int drag_x, drag_y, drag_cx, drag_cy, drag_w, drag_h;
static unsigned drag_button;

static void event(XEvent *e) {
    Client *c;
    if (randr &&
        (e->type == randr_event + RRScreenChangeNotify || e->type == randr_event + RRNotify)) {
        XRRUpdateConfiguration(e);
        discover_monitors(false);
        return;
    }
    switch (e->type) {
    case MapRequest:
        manage(e->xmaprequest.window);
        break;
    case DestroyNotify:
        c = find(e->xdestroywindow.window);
        if (c)
            unmanage(c, true);
        break;
    case UnmapNotify:
        c = find(e->xunmap.window);
        if (c) {
            if (c->ignore_unmap && !e->xunmap.send_event)
                --c->ignore_unmap;
            else
                unmanage(c, false);
        }
        break;
    case ConfigureRequest: {
        XConfigureRequestEvent *r = &e->xconfigurerequest;
        c = find(r->window);
        if (!c) {
            XWindowChanges wc = {.x = r->x,
                                 .y = r->y,
                                 .width = r->width,
                                 .height = r->height,
                                 .border_width = r->border_width,
                                 .sibling = r->above,
                                 .stack_mode = r->detail};
            XConfigureWindow(dpy, r->window, (unsigned)r->value_mask, &wc);
        } else if (!c->fullscreen &&
                   (c->floating || !strcmp(config.layouts[c->workspace], "float"))) {
            resize(c, r->value_mask & CWX ? r->x : c->x, r->value_mask & CWY ? r->y : c->y,
                   r->value_mask & CWWidth ? r->width : c->w,
                   r->value_mask & CWHeight ? r->height : c->h);
        } else
            notify_configure(c);
        break;
    }
    case ConfigureNotify:
        if (e->xconfigure.window == root)
            discover_monitors(false);
        break;
    case PropertyNotify:
        c = find(e->xproperty.window);
        if (c && e->xproperty.atom == net_type) {
            update_window_type(c);
        } else if (c && (e->xproperty.atom == XA_WM_NAME || e->xproperty.atom == net_name)) {
            title(c);
            emit("title", c);
        } else if (c && e->xproperty.atom == XA_WM_NORMAL_HINTS) {
            read_hints(c);
            arrange();
        } else if (c && c->dock &&
                   (e->xproperty.atom == net_strut || e->xproperty.atom == net_strut_partial)) {
            arrange();
            emit("workarea", c);
        }
        break;
    case EnterNotify:
        if (config.sloppy && e->xcrossing.mode == NotifyNormal &&
            e->xcrossing.detail != NotifyInferior) {
            c = find(e->xcrossing.window);
            if (c && visible(c))
                focus(c);
        }
        break;
    case KeyPress: {
        unsigned mods = e->xkey.state & ~(numlock_mask | LockMask);
        for (int i = 0; i < config.nbindings; ++i) {
            Binding *b = &config.bindings[i];
            if (e->xkey.keycode != XKeysymToKeycode(dpy, b->key) || mods != b->mods)
                continue;
            lua_binding(b);
            break;
        }
        break;
    }
    case MappingNotify:
        XRefreshKeyboardMapping(&e->xmapping);
        grabkeys();
        break;
    case ButtonPress:
        c = find(e->xbutton.window);
        if (c) {
            focus(c);
            if ((e->xbutton.state & Mod4Mask) && !c->fullscreen &&
                (e->xbutton.button == Button1 || e->xbutton.button == Button3)) {
                XAllowEvents(dpy, AsyncPointer, CurrentTime);
                if (XGrabPointer(dpy, root, False, PointerMotionMask | ButtonReleaseMask,
                                 GrabModeAsync, GrabModeAsync, None, None,
                                 CurrentTime) == GrabSuccess) {
                    drag_window = c->win;
                    drag_x = e->xbutton.x_root;
                    drag_y = e->xbutton.y_root;
                    drag_cx = c->x;
                    drag_cy = c->y;
                    drag_w = c->w;
                    drag_h = c->h;
                    drag_button = e->xbutton.button;
                    c->floating = true;
                    XRaiseWindow(dpy, c->win);
                    arrange();
                    emit("floating", c);
                }
            } else
                XAllowEvents(dpy, ReplayPointer, CurrentTime);
        }
        break;
    case MotionNotify:
        c = find(drag_window);
        if (c) {
            int dx = e->xmotion.x_root - drag_x, dy = e->xmotion.y_root - drag_y;
            if (drag_button == Button1)
                resize(c, drag_cx + dx, drag_cy + dy, drag_w, drag_h);
            else
                resize(c, drag_cx, drag_cy, maximum(32, drag_w + dx), maximum(32, drag_h + dy));
        }
        break;
    case ButtonRelease:
        if (drag_window) {
            drag_window = None;
            XUngrabPointer(dpy, CurrentTime);
        }
        break;
    case ClientMessage:
        c = find(e->xclient.window);
        if (e->xclient.format != 32)
            break;
        if (e->xclient.message_type == net_current) {
            long ws = e->xclient.data.l[0];
            if (ws >= 0 && ws < config.count) {
                char arg[16];
                snprintf(arg, sizeof arg, "%ld", ws + 1);
                wm_command("workspace", arg);
            }
        } else if (c && e->xclient.message_type == net_active) {
            char arg[16];
            snprintf(arg, sizeof arg, "%d", c->workspace + 1);
            wm_command("workspace", arg);
            focus(c);
        } else if (c && !c->dock && e->xclient.message_type == net_wm_desktop) {
            long ws = e->xclient.data.l[0];
            if (ws >= 0 && ws < config.count) {
                c->workspace = (int)ws;
                property(c->win, net_wm_desktop, (unsigned long)ws);
                arrange();
                focus(NULL);
                emit("workspace", c);
            }
        } else if (c && e->xclient.message_type == net_state &&
                   ((Atom)e->xclient.data.l[1] == net_fullscreen ||
                    (Atom)e->xclient.data.l[2] == net_fullscreen)) {
            long action = e->xclient.data.l[0];
            if (action >= 0 && action <= 2)
                fullscreen(c, action == 2 ? !c->fullscreen : action == 1);
        }
        break;
    default:
        break;
    }
}

static bool wm_conflict;

static int startup_error(Display *display, XErrorEvent *e) {
    (void)display;
    (void)e;
    wm_conflict = true;
    return 0;
}

static int xerror(Display *display, XErrorEvent *e) {
    if (e->error_code == BadWindow || e->error_code == BadDrawable || e->error_code == BadMatch)
        return 0;
    char message[256];
    XGetErrorText(display, e->error_code, message, sizeof message);
    fprintf(stderr, "mori: X11: %s (request %u)\n", message, e->request_code);
    return 0;
}

static void signal_stop(int sig) {
    (void)sig;
    stopped = 1;
}

static void atoms(void) {
#define ATOM(var, name) var = XInternAtom(dpy, name, False)
    ATOM(wm_protocols, "WM_PROTOCOLS");
    ATOM(wm_delete, "WM_DELETE_WINDOW");
    ATOM(wm_take_focus, "WM_TAKE_FOCUS");
    ATOM(wm_state, "WM_STATE");
    ATOM(utf8, "UTF8_STRING");
    ATOM(net_supported, "_NET_SUPPORTED");
    ATOM(net_support, "_NET_SUPPORTING_WM_CHECK");
    ATOM(net_name, "_NET_WM_NAME");
    ATOM(net_active, "_NET_ACTIVE_WINDOW");
    ATOM(net_clients, "_NET_CLIENT_LIST");
    ATOM(net_desktops, "_NET_NUMBER_OF_DESKTOPS");
    ATOM(net_current, "_NET_CURRENT_DESKTOP");
    ATOM(net_names, "_NET_DESKTOP_NAMES");
    ATOM(net_wm_desktop, "_NET_WM_DESKTOP");
    ATOM(net_state, "_NET_WM_STATE");
    ATOM(net_fullscreen, "_NET_WM_STATE_FULLSCREEN");
    ATOM(net_type, "_NET_WM_WINDOW_TYPE");
    ATOM(net_dialog, "_NET_WM_WINDOW_TYPE_DIALOG");
    ATOM(net_dock, "_NET_WM_WINDOW_TYPE_DOCK");
    ATOM(net_workarea, "_NET_WORKAREA");
    ATOM(net_strut, "_NET_WM_STRUT");
    ATOM(net_strut_partial, "_NET_WM_STRUT_PARTIAL");
#undef ATOM
    Atom supported[] = {net_supported, net_workarea,     net_support, net_name,   net_active,
                        net_clients,   net_desktops,     net_current, net_names,  net_wm_desktop,
                        net_state,     net_fullscreen,   net_type,    net_dialog, net_dock,
                        net_strut,     net_strut_partial};
    XChangeProperty(dpy, root, net_supported, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)supported, (int)LENGTH(supported));
    support = XCreateSimpleWindow(dpy, root, -1, -1, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, root, net_support, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&support, 1);
    XChangeProperty(dpy, support, net_support, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&support, 1);
    XChangeProperty(dpy, support, net_name, utf8, 8, PropModeReplace, (unsigned char *)"Mori", 4);
}

static void cleanup(void) {
    ipc_stop();
    if (dpy) {
        XUngrabKey(dpy, AnyKey, AnyModifier, root);
        XUngrabPointer(dpy, CurrentTime);
        while (clients) {
            Client *c = clients;
            clients = c->next;
            XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
            XSetWindowBorderWidth(dpy, c->win, 0);
            XMapWindow(dpy, c->win);
            XRemoveFromSaveSet(dpy, c->win);
            free(c);
        }
        Atom props[] = {net_supported, net_support, net_active, net_clients,
                        net_desktops,  net_current, net_names,  net_workarea};
        for (size_t i = 0; i < LENGTH(props); ++i)
            if (props[i])
                XDeleteProperty(dpy, root, props[i]);
        if (support)
            XDestroyWindow(dpy, support);
        XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
        XCloseDisplay(dpy);
    }
    if (config.lua)
        lua_close(config.lua);
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--check-config")) {
        if (argc != 3) {
            fprintf(stderr, "usage: mori --check-config PATH\n");
            return 2;
        }
        Config c;
        if (!lua_config_load(&c, argv[2], false))
            return 1;
        lua_close(c.lua);
        puts("configuration valid");
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: mori [--check-config PATH]\n");
        return 2;
    }
    const char *home = getenv("HOME");
    if (!home || snprintf(config_path, sizeof config_path, "%s/.config/mori/config.lua", home) >=
                     (int)sizeof config_path) {
        fprintf(stderr, "mori: invalid HOME\n");
        return 1;
    }
    if (!lua_config_load(&config, config_path, true))
        return 1;
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "mori: cannot open X display\n");
        lua_close(config.lua);
        return 1;
    }
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    XSetErrorHandler(startup_error);
    XSelectInput(dpy, root,
                 SubstructureRedirectMask | SubstructureNotifyMask | StructureNotifyMask);
    XSync(dpy, False);
    if (wm_conflict) {
        fprintf(stderr, "mori: another window manager owns this display\n");
        XCloseDisplay(dpy);
        lua_close(config.lua);
        return 1;
    }
    XSetErrorHandler(xerror);
    if (!colors(&config, &normal_pixel, &focus_pixel) || !ipc_start()) {
        cleanup();
        return 1;
    }
    struct sigaction sa = {.sa_handler = signal_stop};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    atoms();
    randr = XRRQueryExtension(dpy, &randr_event, &randr_error);
    if (randr)
        XRRSelectInput(dpy, root,
                       RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
                           RROutputChangeNotifyMask);
    discover_monitors(true);
    grabkeys();
    Window dummy_root, dummy_parent, *children = NULL;
    unsigned nchildren = 0;
    if (XQueryTree(dpy, root, &dummy_root, &dummy_parent, &children, &nchildren)) {
        for (unsigned i = 0; i < nchildren; ++i) {
            XWindowAttributes a;
            if (XGetWindowAttributes(dpy, children[i], &a) && a.map_state == IsViewable)
                manage(children[i]);
        }
        if (children)
            XFree(children);
    }
    lua_startup();
    emit("startup", NULL);
    while (running && !stopped) {
        /* Limit X processing so IPC remains responsive under event floods. */
        for (int budget = 256; budget && XPending(dpy); --budget) {
            XEvent e;
            XNextEvent(dpy, &e);
            event(&e);
        }
        if (reload_pending) {
            reload_pending = false;
            wm_reload();
        }
        XFlush(dpy);
        struct pollfd fds[1 + IPC_POLL_COUNT];
        fds[0] = (struct pollfd){.fd = ConnectionNumber(dpy), .events = POLLIN};
        ipc_prepare(fds + 1);
        if (!running || stopped)
            break;
        int rc = poll(fds, LENGTH(fds), XPending(dpy) ? 0 : 250);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("mori: poll");
            break;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        ipc_dispatch(fds + 1);
    }
    cleanup();
    return 0;
}
