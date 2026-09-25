-- Optional, separate process: copy to ~/.config/mori/bar.lua and run mori-bar.
-- Reload with: kill -HUP $(pgrep -x mori-bar)
return {
    height = 26, -- pixels, 1..256
    padding = 8, -- left text padding, 0..256
    position = "top", -- top or bottom
    monitor = "", -- RandR output name; empty/unavailable selects the first monitor
    font = "monospace", -- fontconfig pattern, e.g. "DejaVu Sans Mono:size=11"
    foreground = "#eeeeee",
    background = "#202020",

    -- Built-in workspace/title appearance. Omitted colors inherit foreground/background.
    workspace_foreground = "#aaaaaa",
    workspace_background = "#202020",
    workspace_active_foreground = "#ffffff",
    workspace_padding = 4, -- each side of an equal-width slot, 0..256
    workspace_spacing = 2, -- gap between slots, 0..256
    active_symbol = "●", -- empty string keeps the focused workspace's name
    show_title = true,
    title_separator = " | ",

    -- X11/XEmbed system tray, aligned right (one owner per X screen).
    tray = true,
    tray_icon_size = 20, -- 1..256; clamped to bar height
    tray_spacing = 4, -- gap between icons, 0..256
    tray_padding = 8, -- horizontal inset on both sides, 0..256
    tray_background = "#202020", -- defaults to background when omitted

    -- Optional formatter, called after each snapshot and on redraw.
    -- State arrays contain the same fields as mori-ipc workspaces/windows/monitors.
    -- Keep callbacks short. Errors fall back to the built-in display.
    -- Omit format to use the built-in centered, equal-width workspace slots.
    -- format = function(state)
    --     local parts = {}
    --     for _, ws in ipairs(state.workspaces or {}) do
    --         parts[#parts + 1] = ws.focused and "●" or ws.name
    --     end
    --     for _, win in ipairs(state.windows or {}) do
    --         if win.focused then parts[#parts + 1] = "| " .. win.title end
    --     end
    --     return table.concat(parts, " ")
    -- end,
}
