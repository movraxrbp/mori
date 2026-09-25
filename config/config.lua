-- Copy to ~/.config/mori/config.lua. This file returns the entire configuration.
local super = { "Super" }
local keys = {
    { modifiers = super, key = "Return", command = "spawn", argument = "xterm" },
    { modifiers = super, key = "p", command = "spawn", argument = "dmenu_run" },
    { modifiers = super, key = "j", command = "focus", argument = "next" },
    { modifiers = super, key = "k", command = "focus", argument = "prev" },
    { modifiers = super, key = "space", command = "floating" },
    { modifiers = super, key = "f", command = "fullscreen" },
    { modifiers = super, key = "t", command = "layout", argument = "tile" },
    { modifiers = super, key = "m", command = "layout", argument = "monocle" },
    { modifiers = super, key = "v", command = "layout", argument = "float" },
    { modifiers = super, key = "period", command = "monitor", argument = "next" },
    { modifiers = super, key = "comma", command = "monitor", argument = "prev" },
    { modifiers = { "Super", "Shift" }, key = "c", command = "close" },
    { modifiers = { "Super", "Shift" }, key = "r", command = "reload" },
    { modifiers = { "Super", "Shift" }, key = "q", command = "quit" },
}
for i = 1, 9 do
    keys[#keys + 1] = { modifiers = super, key = tostring(i), command = "workspace", argument = tostring(i) }
    keys[#keys + 1] = { modifiers = { "Super", "Shift" }, key = tostring(i), command = "move-workspace", argument = tostring(i) }
end

return {
    workspaces = { "1", "2", "3", "4", "5", "6", "7", "8", "9" },
    layout = "tile", -- tile, monocle, float; each workspace can change independently
    master_count = 1,
    master_ratio = 0.55,
    gaps = 8,
    border_width = 2,
    border_normal = "#444444",
    border_focus = "#88c0d0",
    focus_follows_mouse = false,
    keybindings = keys,
    rules = {
        { class = "Pavucontrol", floating = true },
        -- { class = "Firefox", workspace = 2 },
        -- { title = "Picture-in-Picture", floating = true },
    },
    monitors = {
        -- { name = "eDP-1", workspace = 1 },
        -- { name = "HDMI-1", workspace = 2 },
    },
    startup = {
        -- "xsetroot -solid '#202020'",
        -- "mori-bar", -- optional; configured separately in ~/.config/mori/bar.lua
    },
    hooks = {
        -- Callbacks receive { event, workspace, monitor, window }.
        -- workspace = function(event) print("workspace", event.workspace) end,
        -- startup = function() mori.spawn("notify-send 'Mori ready'") end,
    },
}
