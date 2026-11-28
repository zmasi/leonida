-- Example Lua mod (docs/lua-modding.md). Disabled in mod.json: set "enabled" to true
-- and press "Reload Lua mods" in the F4 menu (Mods), or restart.
--
-- It shows what the API offers:
--   * an overlay line in the lower left corner: frames a second and the run time;
--   * a page "Example Lua mod" in the F4 menu (Mods) with a toggle, a number and actions;
--   * F9 flips the line on and off.

leonida.log("started, API version", leonida.api_version)

local show = true
local size = 20
local fps, frames, since = 0, 0, leonida.time()

leonida.on("frame", function(dt)
  -- A rate averaged over half a second, so the digits do not flicker.
  frames = frames + 1
  local now = leonida.time()
  if now - since >= 0.5 then
    fps = frames / (now - since)
    frames, since = 0, now
  end
  if show then
    -- Coordinates are pixels of a 1080-line picture; they scale with the real one.
    leonida.rect(24, 1034, 520, size + 12, {0, 0, 0, 150})
    leonida.text(32, 1040, string.format("Lua example   %.1f fps   %.1f s   frame %d", fps, now, leonida.frame()),
                 { size = size, color = "#FFE08A" })
  end
end)

local page = leonida.menu.page("Example Lua mod")

local toggle = page:toggle("Show the line", show, function(on)
  show = on
end, "The frame-rate line in the lower left corner (F9 too).")

page:number("Line size", size, { min = 12, max = 48, step = 2, suffix = " px" }, function(value)
  size = value
end)

page:action("Take a screenshot", function()
  local path, why = leonida.screenshot()
  leonida.log("screenshot:", path or why)
end, "Saves the next frame as a PNG (diagnostics builds).")

page:action("Log the image base", function()
  local base, length = leonida.memory.base()
  if base then
    leonida.log(string.format("the title's image: %#x, %d bytes; its first dword: %s", base, length,
                              tostring(leonida.memory.read_u32(base))))
  else
    leonida.log("the title's image is not known")
  end
end, "Reads the first dword of the title's executable image through the checked path.")

leonida.on("key", function(key, down)
  if key == "F9" and down then
    show = not show
    toggle:set(show)
  end
end)

leonida.on("menu_open", function()
  leonida.log("the F4 menu was opened")
end)
