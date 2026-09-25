#include "mori.h"
#include <lauxlib.h>
#include <lualib.h>
#include <errno.h>
#include <unistd.h>

static bool loading, in_hook;

static int lua_spawn(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    if (loading)
        return luaL_error(L, "mori.spawn is only available in callbacks");
    wm_spawn(s);
    return 0;
}

static int lua_command(lua_State *L) {
    const char *cmd = luaL_checkstring(L, 1), *arg = luaL_optstring(L, 2, "");
    if (loading)
        return luaL_error(L, "mori.command is only available in callbacks");
    const char *error = wm_command(cmd, arg);
    lua_pushboolean(L, !error);
    if (error) {
        lua_pushstring(L, error);
        return 2;
    }
    return 1;
}

static int integer(lua_State *L, int idx, const char *key, int fallback, int lo, int hi) {
    lua_getfield(L, idx, key);
    int value = fallback;
    if (!lua_isnil(L, -1)) {
        if (!lua_isinteger(L, -1))
            luaL_error(L, "%s must be an integer", key);
        lua_Integer v = lua_tointeger(L, -1);
        if (v < lo || v > hi)
            luaL_error(L, "%s must be between %d and %d", key, lo, hi);
        value = (int)v;
    }
    lua_pop(L, 1);
    return value;
}

static bool boolean(lua_State *L, int idx, const char *key, bool fallback) {
    lua_getfield(L, idx, key);
    if (!lua_isnil(L, -1) && !lua_isboolean(L, -1))
        luaL_error(L, "%s must be boolean", key);
    bool v = lua_isnil(L, -1) ? fallback : lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

static void field(lua_State *L, int idx, const char *key, char *dst, size_t cap,
                  const char *fallback) {
    lua_getfield(L, idx, key);
    const char *s = lua_isnil(L, -1) ? fallback : luaL_checkstring(L, -1);
    if (strlen(s) >= cap)
        luaL_error(L, "%s is too long", key);
    copy(dst, cap, s);
    lua_pop(L, 1);
}

static unsigned modifier(lua_State *L, const char *s) {
    if (!strcmp(s, "Super") || !strcmp(s, "Mod4"))
        return Mod4Mask;
    if (!strcmp(s, "Alt") || !strcmp(s, "Mod1"))
        return Mod1Mask;
    if (!strcmp(s, "Shift"))
        return ShiftMask;
    if (!strcmp(s, "Control"))
        return ControlMask;
    luaL_error(L, "unknown modifier: %s", s);
    return 0;
}

/* Validation runs inside lua_pcall so malformed configuration never panics. */
static int parse_config(lua_State *L) {
    Config *c = lua_touserdata(L, lua_upvalueindex(1));
    luaL_checktype(L, 1, LUA_TTABLE);
    c->gap = integer(L, 1, "gaps", 8, 0, 200);
    c->border = integer(L, 1, "border_width", 2, 0, 32);
    c->masters = integer(L, 1, "master_count", 1, 1, 16);
    c->sloppy = boolean(L, 1, "focus_follows_mouse", false);
    lua_getfield(L, 1, "master_ratio");
    c->ratio = lua_isnil(L, -1) ? .55 : luaL_checknumber(L, -1);
    if (!(c->ratio >= .1 && c->ratio <= .9))
        return luaL_error(L, "master_ratio must be between 0.1 and 0.9");
    lua_pop(L, 1);
    field(L, 1, "border_normal", c->normal, sizeof c->normal, "#444444");
    field(L, 1, "border_focus", c->focus, sizeof c->focus, "#88c0d0");
    char layout[16];
    field(L, 1, "layout", layout, sizeof layout, "tile");
    if (!valid_layout(layout))
        return luaL_error(L, "unknown layout");
    lua_getfield(L, 1, "workspaces");
    if (lua_isnil(L, -1))
        c->count = 9;
    else {
        luaL_checktype(L, -1, LUA_TTABLE);
        c->count = (int)lua_rawlen(L, -1);
        if (c->count < 1 || c->count > MAX_WS)
            return luaL_error(L, "workspaces needs 1..32 names");
    }
    for (int i = 0; i < c->count; ++i) {
        snprintf(c->names[i], sizeof c->names[i], "%d", i + 1);
        copy(c->layouts[i], sizeof c->layouts[i], layout);
        if (!lua_isnil(L, -1)) {
            lua_rawgeti(L, -1, i + 1);
            const char *name = luaL_checkstring(L, -1);
            if (strlen(name) >= sizeof c->names[i])
                return luaL_error(L, "workspace name too long");
            copy(c->names[i], sizeof c->names[i], name);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "keybindings");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        if (lua_rawlen(L, -1) > MAX_BIND)
            return luaL_error(L, "too many keybindings");
        c->nbindings = (int)lua_rawlen(L, -1);
        for (int i = 0; i < c->nbindings; ++i) {
            Binding *b = &c->bindings[i];
            b->callback = LUA_NOREF;
            lua_rawgeti(L, -1, i + 1);
            luaL_checktype(L, -1, LUA_TTABLE);
            char key[128];
            field(L, lua_gettop(L), "key", key, sizeof key, "");
            b->key = XStringToKeysym(key);
            if (b->key == NoSymbol)
                return luaL_error(L, "unknown key: %s", key);
            field(L, lua_gettop(L), "command", b->command, sizeof b->command, "");
            field(L, lua_gettop(L), "argument", b->argument, sizeof b->argument, "");
            lua_getfield(L, -1, "action");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TFUNCTION);
                b->callback = luaL_ref(L, LUA_REGISTRYINDEX);
            } else
                lua_pop(L, 1);
            if (!b->command[0] && b->callback == LUA_NOREF)
                return luaL_error(L, "binding needs command or action");
            lua_getfield(L, -1, "modifiers");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TTABLE);
                for (size_t j = 1; j <= lua_rawlen(L, -1); ++j) {
                    lua_rawgeti(L, -1, (lua_Integer)j);
                    b->mods |= modifier(L, luaL_checkstring(L, -1));
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 2);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "rules");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        if (lua_rawlen(L, -1) > MAX_RULE)
            return luaL_error(L, "too many rules");
        c->nrules = (int)lua_rawlen(L, -1);
        for (int i = 0; i < c->nrules; ++i) {
            Rule *r = &c->rules[i];
            lua_rawgeti(L, -1, i + 1);
            luaL_checktype(L, -1, LUA_TTABLE);
            int t = lua_gettop(L);
            field(L, t, "class", r->class, sizeof r->class, "");
            field(L, t, "instance", r->instance, sizeof r->instance, "");
            field(L, t, "title", r->title, sizeof r->title, "");
            r->workspace = integer(L, t, "workspace", 0, 1, c->count) - 1;
            lua_getfield(L, t, "floating");
            r->floating = lua_isnil(L, -1) ? -1 : boolean(L, t, "floating", false);
            lua_pop(L, 1);
            lua_getfield(L, t, "fullscreen");
            r->fullscreen = lua_isnil(L, -1) ? -1 : boolean(L, t, "fullscreen", false);
            lua_pop(L, 1);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "monitors");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        if (lua_rawlen(L, -1) > MAX_MON)
            return luaL_error(L, "too many monitors");
        c->nmonitors = (int)lua_rawlen(L, -1);
        for (int i = 0; i < c->nmonitors; ++i) {
            lua_rawgeti(L, -1, i + 1);
            luaL_checktype(L, -1, LUA_TTABLE);
            int t = lua_gettop(L);
            field(L, t, "name", c->monitors[i].name, sizeof c->monitors[i].name, "");
            c->monitors[i].workspace = integer(L, t, "workspace", 1, 1, c->count) - 1;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "startup");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        for (size_t i = 1; i <= lua_rawlen(L, -1); ++i) {
            lua_rawgeti(L, -1, (lua_Integer)i);
            luaL_checktype(L, -1, LUA_TSTRING);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "hooks");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            luaL_checktype(L, -2, LUA_TSTRING);
            luaL_checktype(L, -1, LUA_TFUNCTION);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_pushvalue(L, 1);
    c->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

bool lua_config_load(Config *c, const char *path, bool optional) {
    memset(c, 0, sizeof *c);
    lua_State *L = c->lua = luaL_newstate();
    if (!L)
        return false;
    luaL_openlibs(L);
    lua_newtable(L);
    lua_pushcfunction(L, lua_spawn);
    lua_setfield(L, -2, "spawn");
    lua_pushcfunction(L, lua_command);
    lua_setfield(L, -2, "command");
    lua_setglobal(L, "mori");
    loading = true;
    int status;
    if (optional && access(path, F_OK) < 0 && errno == ENOENT) {
        lua_newtable(L);
        status = LUA_OK;
    } else {
        status = luaL_loadfile(L, path);
        if (!status)
            status = lua_pcall(L, 0, 1, 0);
    }
    if (!status) {
        lua_pushlightuserdata(L, c);
        lua_pushcclosure(L, parse_config, 1);
        lua_insert(L, -2);
        status = lua_pcall(L, 1, 0, 0);
    }
    loading = false;
    if (status) {
        fprintf(stderr, "mori: config: %s\n", lua_tostring(L, -1));
        lua_close(L);
        c->lua = NULL;
        return false;
    }
    return true;
}

static void call(lua_State *L, int nargs) {
    if (lua_pcall(L, nargs, 0, 0)) {
        fprintf(stderr, "mori: callback: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

void lua_hook(const char *event, const Client *c) {
    if (!config.lua || in_hook)
        return;
    lua_State *L = config.lua;
    int base = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, config.ref);
    lua_getfield(L, -1, "hooks");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, event);
        if (lua_isfunction(L, -1)) {
            lua_newtable(L);
            lua_pushstring(L, event);
            lua_setfield(L, -2, "event");
            lua_pushinteger(L, monitors[selected_monitor].workspace + 1);
            lua_setfield(L, -2, "workspace");
            lua_pushinteger(L, selected_monitor + 1);
            lua_setfield(L, -2, "monitor");
            lua_pushinteger(L, c ? (lua_Integer)c->win : 0);
            lua_setfield(L, -2, "window");
            in_hook = true;
            call(L, 1);
            in_hook = false;
        }
    }
    lua_settop(L, base);
}

void lua_binding(const Binding *b) {
    if (b->callback != LUA_NOREF) {
        lua_rawgeti(config.lua, LUA_REGISTRYINDEX, b->callback);
        call(config.lua, 0);
    } else {
        const char *error = wm_command(b->command, b->argument);
        if (error)
            fprintf(stderr, "mori: %s\n", error);
    }
}

void lua_startup(void) {
    lua_State *L = config.lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, config.ref);
    lua_getfield(L, -1, "startup");
    if (lua_istable(L, -1))
        for (size_t i = 1; i <= lua_rawlen(L, -1); ++i) {
            lua_rawgeti(L, -1, (lua_Integer)i);
            wm_spawn(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    lua_pop(L, 2);
}
