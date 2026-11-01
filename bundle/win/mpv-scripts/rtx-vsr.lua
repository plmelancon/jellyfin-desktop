-- RTX Video Super Resolution for Jellyfin Media Player.
-- Adds mpv's d3d11vpp filter in NVIDIA mode when the video is smaller than the screen.
-- Skips: HDR, anything not decoded by d3d11va, video already at screen size.
local LABEL = "@rtxvsr"
local current = nil
local timer = nil

local function target_size()
    local w = mp.get_property_number("display-width", 0)
    local h = mp.get_property_number("display-height", 0)
    if w > 0 and h > 0 then return w, h end
    return mp.get_property_number("osd-width", 0), mp.get_property_number("osd-height", 0)
end

local function wanted_filter()
    if mp.get_property("hwdec-current") ~= "d3d11va" then return nil, "not d3d11va" end
    local p = mp.get_property_native("video-dec-params")
    if not p or not p.w or not p.h or p.w <= 0 or p.h <= 0 then return nil, "no video" end
    if p.gamma == "pq" or p.gamma == "hlg" then return nil, "HDR" end
    local tw, th = target_size()
    if tw <= 0 or th <= 0 then return nil, "no target size" end
    local scale = math.min(tw / p.w, th / p.h)
    if scale <= 1.05 then return nil, "already at screen size" end
    -- Quarter steps, so small window size changes do not rebuild the filter.
    scale = math.min(math.ceil(scale * 4) / 4, 4)
    -- VSR needs 8-bit input; 10-bit SDR is converted.
    local fmt = p["hw-pixelformat"] == "nv12" and "" or "format=nv12:"
    return string.format("%s:d3d11vpp=%sscale=%.2f:scaling-mode=nvidia", LABEL, fmt, scale), nil
end

local function update()
    local want, why = wanted_filter()
    if want == current then return end
    if current then mp.commandv("vf", "remove", LABEL) end
    current = nil
    if want then
        if mp.commandv("vf", "add", want) then
            current = want
            mp.msg.info("VSR on: " .. want)
        else
            mp.msg.warn("VSR could not be added: " .. want)
        end
    else
        mp.msg.info("VSR off: " .. tostring(why))
    end
end

local function schedule()
    if timer then timer:kill() end
    timer = mp.add_timeout(0.5, update)
end

mp.observe_property("hwdec-current", "string", schedule)
mp.observe_property("video-dec-params", "native", schedule)
mp.observe_property("osd-dimensions", "native", schedule)
mp.register_event("end-file", function() current = nil end)
