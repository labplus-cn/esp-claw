-- labplus_ledong_v2 smart-home scene
--
-- This file is baked into /system/.recovery/main.lua and copied to the DATA
-- root only when main.lua is missing. Users can therefore replace DATA/main.lua
-- without a firmware update overwriting their copy.

local lvgl = require("lvgl")
local delay = require("delay")
local system = require("system")
local board_manager = require("board_manager")

local function optional_require(name)
    local ok, module_or_error = pcall(require, name)
    if ok then
        return module_or_error
    end
    print(string.format("[smart_home] module %s unavailable: %s", name, tostring(module_or_error)))
    return nil
end

local environmental_sensor = optional_require("environmental_sensor")
local sht20 = optional_require("sht20")
local imu = optional_require("imu")
local audio = optional_require("audio")
local stm8 = optional_require("stm8")
local led_strip = optional_require("led_strip")
local button = optional_require("button")
local ledc = optional_require("ledc")

local CFG = {
    loop_delay_ms = 60,
    audio_sample_ms = 40,
    climate_sample_loops = 10,
    sht20_port = 0,
    sht20_sda = 44,
    sht20_scl = 43,
    sht20_addr = 0x40,
    sht20_frequency = 100000,
    fan_on_temperature_c = 30,
    fan_hot_speed = 70,
    humidity_close_percent = 70,
    light_on_lux = 200,
    light_off_lux = 500,
    sound_min_peak = 3500,
    sound_min_rms = 1000,
    noise_min_rms = 6000,
    noise_confirm_samples = 3,
    sound_reduction_hold_samples = 24,
    accel_delta_alarm = 2000,       -- QMI8658 Lua unit: mm/s^2
    vibration_confirm_samples = 2,
    vibration_hold_samples = 12,
    alarm_duration_ms = 3000,
    led_count = 3,
    buzzer_gpio = 21,
    buzzer_frequency_hz = 2200,
    buzzer_duty_percent = 35,
}

local state = {
    lux = 0,
    light_valid = false,
    temperature = 0,
    humidity = 0,
    climate_valid = false,
    too_hot = false,
    too_humid = false,
    sound_rms = 0,
    sound_peak = 0,
    sound_percent = 0,
    sound_baseline = nil,
    sound_hold = 0,
    noise_count = 0,
    noise_reduction = false,
    vibration_score = 0,
    vibration_count = 0,
    vibration_hold = 0,
    vibration_alarm = false,
    alarm_until_ms = 0,
    imu_baseline = nil,
    auto_light = false,
    auto_window = true,
    auto_curtain = false,
    light_on = false,
    fan_speed = 0,
    door_open = false,
    window_open = false,
    curtain_open = false,
    selected = 1,
    manual = {},
    loop_count = 0,
}

local selection = {
    { key = "light", label = "灯" },
    { key = "fan", label = "风扇" },
    { key = "door", label = "门" },
    { key = "window", label = "窗" },
    { key = "curtain", label = "窗帘" },
}

local resources = {}
local open_faults = {}
local read_failures = {}
local ui = nil
local lvgl_started = false

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

local function approach(value, target, step)
    if value < target then return math.min(value + step, target) end
    if value > target then return math.max(value - step, target) end
    return value
end

local function try_open(label, fn)
    local ok, result = pcall(fn)
    if ok and result ~= nil then
        print("[smart_home] " .. label .. " ready")
        return result
    end
    open_faults[label] = tostring(result)
    print(string.format("[smart_home] %s disabled: %s", label, tostring(result)))
    return nil
end

local function note_read_result(label, ok, err)
    if ok then
        if read_failures[label] then
            print("[smart_home] " .. label .. " recovered")
        end
        read_failures[label] = nil
        return
    end
    local count = (read_failures[label] or 0) + 1
    read_failures[label] = count
    if count == 1 or count % 100 == 0 then
        print(string.format("[smart_home] %s read failed (%d): %s", label, count, tostring(err)))
    end
end

local function alarm_active()
    if not state.vibration_alarm then return false end
    if system.millis() >= state.alarm_until_ms then
        state.vibration_alarm = false
        state.alarm_until_ms = 0
        return false
    end
    return true
end

local function clear_vibration_alarm()
    state.vibration_alarm = false
    state.alarm_until_ms = 0
    state.vibration_count = 0
end

local function create_scene()
    lvgl.init({
        buffer_lines = 24,
        tick_ms = 5,
        task_period_ms = 10,
        font_size = 14,
    })
    lvgl_started = true

    local scr = lvgl.create_screen()
    scr:set_style({ bg_color = "#07131f", pad = 0 })

    local header = lvgl.object(scr, {
        x = 0, y = 0, w = 320, h = 20,
        bg_color = "#12334a", border_width = 0, radius = 0, pad = 0,
    })
    local title = lvgl.label(header, {
        x = 7, y = 0, text = "智能家居", text_color = "#ffffff",
    })
    local readings = lvgl.label(header, {
        align = "top_right", x = -7, y = 0,
        text = "--°C --%  --lx", text_color = "#bce7ff",
    })

    -- Roof and cross-section house body.
    lvgl.line(scr, {
        points = {{x = 4, y = 33}, {x = 160, y = 20}, {x = 316, y = 33}},
        line_color = "#d26a3c", line_width = 4,
    })
    local house = lvgl.object(scr, {
        x = 4, y = 32, w = 312, h = 115,
        bg_color = "#273641", border_color = "#d9b58c", border_width = 2,
        radius = 1, pad = 0,
    })
    local floor = lvgl.object(house, {
        x = 0, y = 104, w = 308, h = 9,
        bg_color = "#8b684d", border_width = 0, radius = 0,
    })

    -- Door: its panel becomes narrow as it opens, exposing the dark doorway.
    local door_frame = lvgl.object(house, {
        x = 9, y = 40, w = 47, h = 66,
        bg_color = "#16191d", border_color = "#d7c0a2", border_width = 3,
        radius = 1, pad = 0,
    })
    local door_panel = lvgl.object(house, {
        x = 14, y = 44, w = 37, h = 61,
        bg_color = "#8b5835", border_color = "#b67a4d", border_width = 2,
        radius = 1, pad = 0,
    })
    local door_knob = lvgl.object(house, {
        x = 43, y = 74, w = 6, h = 6,
        bg_color = "#f4d35e", border_width = 0, radius = 3,
    })
    local door_label = lvgl.label(house, {
        x = 14, y = 20, text = "门 · 关", text_color = "#f2dfca",
    })

    -- Window and two independent sash panels.
    local window_frame = lvgl.object(house, {
        x = 63, y = 33, w = 96, h = 59,
        bg_color = "#142b3d", border_color = "#e9f2f6", border_width = 3,
        radius = 1, pad = 0,
    })
    local sash_left = lvgl.object(house, {
        x = 68, y = 38, w = 42, h = 49,
        bg_color = "#75bde0", border_color = "#d9f3ff", border_width = 2,
        radius = 0, pad = 0,
    })
    local sash_right = lvgl.object(house, {
        x = 112, y = 38, w = 42, h = 49,
        bg_color = "#75bde0", border_color = "#d9f3ff", border_width = 2,
        radius = 0, pad = 0,
    })
    local window_label = lvgl.label(house, {
        x = 84, y = 14, text = "窗 · 关", text_color = "#d8f4ff",
    })

    -- Two curtains slide from the center toward the edges.
    local curtain_left = lvgl.object(house, {
        x = 65, y = 31, w = 45, h = 64,
        bg_color = "#9256b5", border_color = "#c69bdd", border_width = 1,
        radius = 3, pad = 0,
    })
    local curtain_right = lvgl.object(house, {
        x = 111, y = 31, w = 45, h = 64,
        bg_color = "#9256b5", border_color = "#c69bdd", border_width = 1,
        radius = 3, pad = 0,
    })
    local curtain_label = lvgl.label(house, {
        x = 76, y = 92, text = "窗帘 · 合", text_color = "#e8cdf5",
    })

    -- Hanging lamp; the room and the physical WS2812 strip follow this state.
    lvgl.line(house, {
        points = {{x = 190, y = 0}, {x = 190, y = 21}},
        line_color = "#9fa9ad", line_width = 2,
    })
    local lamp_shade = lvgl.object(house, {
        x = 174, y = 18, w = 33, h = 14,
        bg_color = "#59636a", border_color = "#8c989e", border_width = 1,
        radius = 7, pad = 0,
    })
    local bulb = lvgl.led(house, {
        x = 181, y = 28, w = 19, h = 19,
        color = "#ffd45c", brightness = 40, on = false,
    })
    local lamp_label = lvgl.label(house, {
        x = 169, y = 48, text = "灯 · 灭", text_color = "#d9e0e4",
    })

    -- Four-blade fan. Alternating '+' and 'x' blade sets gives a visible
    -- rotation animation without image assets.
    local fan_plus = {
        lvgl.line(house, { points = {{x=248,y=38},{x=248,y=70}}, line_color="#79c7ff", line_width=7 }),
        lvgl.line(house, { points = {{x=232,y=54},{x=264,y=54}}, line_color="#79c7ff", line_width=7 }),
    }
    local fan_cross = {
        lvgl.line(house, { points = {{x=236,y=42},{x=260,y=66}}, line_color="#79c7ff", line_width=7, opa=0 }),
        lvgl.line(house, { points = {{x=260,y=42},{x=236,y=66}}, line_color="#79c7ff", line_width=7, opa=0 }),
    }
    local fan_hub = lvgl.object(house, {
        x = 241, y = 47, w = 15, h = 15,
        bg_color = "#dcebf2", border_color = "#6d7f89", border_width = 2,
        radius = 8, pad = 0,
    })
    local fan_label = lvgl.label(house, {
        x = 218, y = 76, text = "风扇 · 停止", text_color = "#cdeaff",
    })

    -- A small plant makes the display read as a room rather than a dashboard.
    local plant_pot = lvgl.object(house, {
        x = 282, y = 86, w = 17, h = 19,
        bg_color = "#a7663e", border_width = 0, radius = 3,
    })
    lvgl.line(house, { points={{x=290,y=86},{x=284,y=72}}, line_color="#62b56f", line_width=5 })
    lvgl.line(house, { points={{x=290,y=86},{x=298,y=70}}, line_color="#62b56f", line_width=5 })

    local footer = lvgl.object(scr, {
        x = 0, y = 149, w = 320, h = 23,
        bg_color = "#12334a", border_width = 0, radius = 0, pad = 0,
    })
    local status = lvgl.label(footer, {
        align = "center", text = "正在初始化传感器…", text_color = "#d7f5df",
    })

    scr:load()
    return {
        screen = scr,
        house = house,
        readings = readings,
        status = status,
        door_frame = door_frame,
        door_panel = door_panel,
        door_knob = door_knob,
        door_label = door_label,
        window_frame = window_frame,
        sash_left = sash_left,
        sash_right = sash_right,
        window_label = window_label,
        curtain_left = curtain_left,
        curtain_right = curtain_right,
        curtain_label = curtain_label,
        lamp_shade = lamp_shade,
        bulb = bulb,
        lamp_label = lamp_label,
        fan_plus = fan_plus,
        fan_cross = fan_cross,
        fan_hub = fan_hub,
        fan_label = fan_label,
        visual = { door = 0, window = 0, curtain = 0, fan_phase = 0 },
    }
end

local function open_hardware()
    if environmental_sensor then
        resources.light = try_open("光线", function()
            return environmental_sensor.new({ type = "ltr308als" })
        end)
    else
        open_faults["光线"] = "module unavailable"
    end

    if sht20 then
        resources.climate = try_open("温湿度", function()
            return sht20.new({
                port = CFG.sht20_port,
                sda = CFG.sht20_sda,
                scl = CFG.sht20_scl,
                address = CFG.sht20_addr,
                frequency = CFG.sht20_frequency,
            })
        end)
    else
        open_faults["温湿度"] = "module unavailable"
    end

    if imu then
        resources.imu = try_open("六轴", function()
            return imu.new("imu_sensor", { accel_only = true })
        end)
    else
        open_faults["六轴"] = "module unavailable"
    end

    if stm8 then
        resources.motor = try_open("风扇电机", function()
            return stm8.new({ device = "stm8s001" })
        end)
    else
        open_faults["风扇电机"] = "module unavailable"
    end

    if led_strip then
        resources.strip = try_open("灯带", function()
            return led_strip.open("board_led_strip")
        end)
    else
        open_faults["灯带"] = "module unavailable"
    end

    if audio then
        resources.audio = try_open("麦克风", function()
            local codec, rate, channels, bits =
                board_manager.get_audio_codec_input_params("audio_adc")
            assert(codec, tostring(rate))
            local input, input_error = audio.new_input({
                codec, rate, channels, bits, volume = 55,
            })
            assert(input, input_error)
            local ok, analyzer_or_error = pcall(audio.analyzer, { input = input })
            if not ok then
                pcall(function() input:close() end)
                error(analyzer_or_error)
            end
            return { input = input, analyzer = analyzer_or_error }
        end)
    else
        open_faults["麦克风"] = "module unavailable"
    end


    if ledc then
        resources.buzzer = try_open("报警蜂鸣器", function()
            return ledc.new({
                gpio = CFG.buzzer_gpio,
                frequency_hz = CFG.buzzer_frequency_hz,
                duty_percent = CFG.buzzer_duty_percent,
                duty_resolution_bits = 10,
            })
        end)
    else
        open_faults["报警蜂鸣器"] = "module unavailable"
    end
end

local function selected_item()
    return selection[state.selected]
end

local function operate_selected()
    local key = selected_item().key
    if key == "fan" then
        local current = state.manual.fan
        if current == nil then current = state.fan_speed end
        local next_speed
        if current < 40 then next_speed = 40
        elseif current < 70 then next_speed = 70
        elseif current < 100 then next_speed = 100
        else next_speed = 0 end
        state.manual.fan = next_speed
        return
    end

    local field = ({
        light = "light_on",
        door = "door_open",
        window = "window_open",
        curtain = "curtain_open",
    })[key]
    state.manual[key] = not state[field]
end

local function setup_buttons()
    if not button then return end
    resources.confirm = try_open("确认键", function()
        return assert(button.open("confirm_button"))
    end)
    resources.back = try_open("返回键", function()
        return assert(button.open("return_button"))
    end)

    if resources.confirm then
        assert(button.on(resources.confirm, "single_click", function()
            state.selected = state.selected % #selection + 1
        end))
        assert(button.on(resources.confirm, "double_click", function()
            operate_selected()
        end))
        assert(button.on(resources.confirm, "long_press_start", function()
            state.manual[selected_item().key] = nil
        end))
    end

    if resources.back then
        assert(button.on(resources.back, "single_click", function()
            clear_vibration_alarm()
        end))
        assert(button.on(resources.back, "long_press_start", function()
            state.manual = {}
            clear_vibration_alarm()
        end))
    end
end

local function sample_climate()
    if not resources.climate then return end
    if state.loop_count % CFG.climate_sample_loops ~= 1 then return end

    local ok, sample = pcall(function() return resources.climate:read() end)
    note_read_result("温湿度", ok, sample)
    if not ok or type(sample) ~= "table" or
       type(sample.temperature) ~= "number" or
       type(sample.humidity) ~= "number" then
        return
    end

    if state.climate_valid then
        state.temperature = state.temperature * 0.7 + sample.temperature * 0.3
        state.humidity = state.humidity * 0.7 + sample.humidity * 0.3
    else
        state.temperature = sample.temperature
        state.humidity = sample.humidity
    end
    state.climate_valid = true
    state.too_hot = state.temperature > CFG.fan_on_temperature_c
    state.too_humid = state.humidity > CFG.humidity_close_percent
end

local function sample_light()
    if not resources.light then return end
    local ok, value = pcall(function() return resources.light:read_lux() end)
    note_read_result("光线", ok, value)
    if ok and type(value) == "number" and value >= 0 then
        if state.light_valid then
            state.lux = state.lux * 0.7 + value * 0.3
        else
            state.lux = value
        end
        state.light_valid = true
    end
end

local function sample_audio()
    if not resources.audio then return end
    local ok, level = pcall(function()
        return resources.audio.analyzer:read_level(CFG.audio_sample_ms)
    end)
    note_read_result("麦克风", ok, level)
    if not ok or type(level) ~= "table" then return end

    state.sound_rms = level.rms or 0
    state.sound_peak = level.peak or 0
    state.sound_percent = math.floor(clamp(state.sound_rms * 100 / 12000, 0, 100) + 0.5)

    if state.sound_baseline == nil then
        state.sound_baseline = math.max(state.sound_rms, 100)
    end
    local baseline = state.sound_baseline
    local trigger_peak = math.max(CFG.sound_min_peak, baseline * 3.5)
    local trigger_rms = math.max(CFG.sound_min_rms, baseline * 2.5)
    local noise_rms = math.max(CFG.noise_min_rms, baseline * 5.0)

    if state.sound_peak >= trigger_peak or state.sound_rms >= trigger_rms then
        state.sound_hold = CFG.sound_reduction_hold_samples
    elseif state.sound_hold > 0 then
        state.sound_hold = state.sound_hold - 1
    end

    if state.sound_rms >= noise_rms then
        state.noise_count = state.noise_count + 1
        if state.noise_count >= CFG.noise_confirm_samples then
            state.noise_reduction = true
        end
    else
        state.noise_count = math.max(state.noise_count - 1, 0)
        if state.sound_hold == 0 and state.noise_count == 0 then
            state.noise_reduction = false
        end
        state.sound_baseline = baseline * 0.985 + state.sound_rms * 0.015
    end
end

local function sample_imu()
    if not resources.imu then return end
    local ok, sample = pcall(function() return resources.imu:read() end)
    note_read_result("六轴", ok, sample)
    if not ok or type(sample) ~= "table" then return end

    local a = sample.accel
    if not a then return end
    if state.imu_baseline == nil then
        state.imu_baseline = { x = a.x, y = a.y, z = a.z }
        return
    end

    local b = state.imu_baseline
    local dx, dy, dz = a.x - b.x, a.y - b.y, a.z - b.z
    local accel_delta = math.sqrt(dx * dx + dy * dy + dz * dz)
    state.vibration_score = accel_delta / CFG.accel_delta_alarm
    local vibrating = accel_delta >= CFG.accel_delta_alarm
    if vibrating then
        state.vibration_count = state.vibration_count + 1
        state.vibration_hold = CFG.vibration_hold_samples
        if state.vibration_count >= CFG.vibration_confirm_samples and
           not state.vibration_alarm then
            state.vibration_alarm = true
            state.alarm_until_ms = system.millis() + CFG.alarm_duration_ms
        end
    else
        state.vibration_count = math.max(state.vibration_count - 1, 0)
        state.vibration_hold = math.max(state.vibration_hold - 1, 0)
        b.x = b.x * 0.98 + a.x * 0.02
        b.y = b.y * 0.98 + a.y * 0.02
        b.z = b.z * 0.98 + a.z * 0.02
    end
end

local function resolve_state(key, automatic)
    local manual = state.manual[key]
    if manual ~= nil then return manual end
    return automatic
end

local function decide_scene()
    if state.light_valid then
        if state.lux < CFG.light_on_lux then
            state.auto_light = true
            state.auto_curtain = false
        elseif state.lux > CFG.light_off_lux then
            state.auto_light = false
            state.auto_curtain = true
        end
    end

    -- Everyday behavior has deterministic priorities. Heat is the only
    -- automatic fan trigger. Humidity and noise both close the window, while
    -- noise and darkness both close the curtains.
    local sound_high = state.sound_hold > 0 or state.noise_reduction
    local auto_fan = state.too_hot and CFG.fan_hot_speed or 0
    local auto_curtain = state.auto_curtain and not sound_high
    state.auto_window = not (state.too_humid or sound_high)

    state.light_on = resolve_state("light", state.auto_light)
    state.fan_speed = resolve_state("fan", auto_fan)
    state.door_open = resolve_state("door", false)
    state.window_open = resolve_state("window", state.auto_window)
    state.curtain_open = resolve_state("curtain", auto_curtain)
end

local last_motor_speed = nil
local last_led_signature = nil
local last_buzzer_on = nil

local function apply_outputs()
    if resources.motor and last_motor_speed ~= state.fan_speed then
        local ok, err = pcall(function() resources.motor:set_motor1(state.fan_speed) end)
        note_read_result("风扇电机", ok, err)
        if ok then last_motor_speed = state.fan_speed end
    end

    if resources.strip then
        local alarm_flash = alarm_active() and (math.floor(state.loop_count / 3) % 2 == 0)
        local signature = string.format("%s:%s", tostring(state.light_on), tostring(alarm_flash))
        if signature ~= last_led_signature then
            local ok, err = pcall(function()
                for index = 0, CFG.led_count - 1 do
                    if alarm_flash then
                        resources.strip:set_pixel(index, 120, 0, 0)
                    elseif state.light_on then
                        resources.strip:set_pixel(index, 96, 48, 10)
                    else
                        resources.strip:set_pixel(index, 0, 0, 0)
                    end
                end
                resources.strip:refresh()
            end)
            note_read_result("灯带", ok, err)
            if ok then last_led_signature = signature end
        end
    end

    if resources.buzzer then
        local buzzer_on = alarm_active() and
                          (math.floor(state.loop_count / 4) % 2 == 0)
        if buzzer_on ~= last_buzzer_on then
            local ok, err = pcall(function()
                if buzzer_on then resources.buzzer:start() else resources.buzzer:stop() end
            end)
            note_read_result("报警蜂鸣器", ok, err)
            if ok then last_buzzer_on = buzzer_on end
        end
    end
end

local function style_selection()
    local selected = selected_item().key
    local alarm_border = alarm_active() and (math.floor(state.loop_count / 3) % 2 == 0)
    ui.lamp_shade:set_style({ border_color = selected == "light" and "#56f39a" or "#8c989e",
                              border_width = selected == "light" and 3 or 1 })
    ui.fan_hub:set_style({ border_color = selected == "fan" and "#56f39a" or "#6d7f89",
                           border_width = selected == "fan" and 3 or 2 })
    ui.door_panel:set_style({ border_color = selected == "door" and "#56f39a" or "#b67a4d",
                              border_width = selected == "door" and 3 or 2 })
    ui.window_frame:set_style({
        border_color = alarm_border and "#ff334d" or
                       (selected == "window" and "#56f39a" or "#e9f2f6"),
        border_width = selected == "window" and 4 or 3,
    })
    local curtain_border = selected == "curtain" and "#56f39a" or "#c69bdd"
    local curtain_width = selected == "curtain" and 3 or 1
    ui.curtain_left:set_style({ border_color = curtain_border, border_width = curtain_width })
    ui.curtain_right:set_style({ border_color = curtain_border, border_width = curtain_width })
end

local function update_scene()
    ui.visual.door = approach(ui.visual.door, state.door_open and 100 or 0, 16)
    ui.visual.window = approach(ui.visual.window, state.window_open and 100 or 0, 16)
    ui.visual.curtain = approach(ui.visual.curtain, state.curtain_open and 100 or 0, 16)

    ui.house:set_style({ bg_color = state.light_on and "#f2dfad" or "#273641" })
    ui.lamp_shade:set_style({ bg_color = state.light_on and "#ffd45c" or "#59636a" })
    ui.bulb:set_color(state.light_on and "#fff09a" or "#66727a")
    ui.bulb:set_brightness(state.light_on and 255 or 32)
    if state.light_on then ui.bulb:on() else ui.bulb:off() end
    ui.lamp_label:set_text(state.light_on and "灯 · 亮" or "灯 · 灭")
    ui.lamp_label:set_style({ text_color = state.light_on and "#6b4a00" or "#d9e0e4" })

    local door_width = 37 - math.floor(ui.visual.door * 28 / 100)
    ui.door_panel:set_size(door_width, 61)
    ui.door_knob:set_pos(14 + math.max(door_width - 8, 2), 74)
    ui.door_label:set_text(state.door_open and "门 · 开" or "门 · 关")
    local alarm_border = alarm_active() and (math.floor(state.loop_count / 3) % 2 == 0)
    ui.door_frame:set_style({ border_color = alarm_border and "#ff334d" or "#d7c0a2" })

    local sash_width = 42 - math.floor(ui.visual.window * 30 / 100)
    local right_x = 112 + math.floor(ui.visual.window * 30 / 100)
    ui.sash_left:set_size(sash_width, 49)
    ui.sash_right:set_pos(right_x, 38)
    ui.sash_right:set_size(sash_width, 49)
    ui.window_label:set_text(state.window_open and "窗 · 开" or "窗 · 关")
    ui.window_frame:set_style({ border_color = alarm_border and "#ff334d" or "#e9f2f6" })
    local sky = state.window_open and "#3a7698" or "#75bde0"
    ui.sash_left:set_style({ bg_color = sky })
    ui.sash_right:set_style({ bg_color = sky })

    local curtain_width = 45 - math.floor(ui.visual.curtain * 34 / 100)
    local curtain_right_x = 111 + math.floor(ui.visual.curtain * 34 / 100)
    ui.curtain_left:set_size(curtain_width, 64)
    ui.curtain_right:set_pos(curtain_right_x, 31)
    ui.curtain_right:set_size(curtain_width, 64)
    ui.curtain_label:set_text(state.curtain_open and "窗帘 · 开" or "窗帘 · 合")

    if state.fan_speed > 0 then
        ui.visual.fan_phase = (ui.visual.fan_phase + state.fan_speed) % 200
    else
        ui.visual.fan_phase = 0
    end
    local plus_visible = state.fan_speed == 0 or ui.visual.fan_phase < 100
    local blade_color = state.fan_speed > 0 and "#52bfff" or "#74838b"
    for _, blade in ipairs(ui.fan_plus) do
        blade:set_style({ line_color = blade_color, opa = plus_visible and 255 or 0 })
    end
    for _, blade in ipairs(ui.fan_cross) do
        blade:set_style({ line_color = blade_color, opa = plus_visible and 0 or 255 })
    end
    ui.fan_label:set_text(state.fan_speed > 0 and
        string.format("风扇 · %d%%", state.fan_speed) or "风扇 · 停止")

    local climate_text = state.climate_valid and
        string.format("%.1f°C %.0f%%", state.temperature, state.humidity) or "--°C --%"
    local lux_text = state.light_valid and
        string.format("%dlx", math.floor(state.lux + 0.5)) or "--lx"
    ui.readings:set_text(climate_text .. "  " .. lux_text)

    local selected = selected_item()
    local mode = state.manual[selected.key] == nil and "自动" or "手动"
    if alarm_active() then
        ui.status:set_text("疑似破门窗！震动报警（3秒）")
        ui.status:set_style({ text_color = "#ffdfdf" })
    elseif state.noise_reduction or state.sound_hold > 0 then
        ui.status:set_text("外部声音较大 | 关窗合帘降噪")
        ui.status:set_style({ text_color = "#ffe99a" })
    elseif state.too_humid then
        ui.status:set_text("湿度过高 | 关窗防潮")
        ui.status:set_style({ text_color = "#9ee8ff" })
    elseif state.too_hot then
        ui.status:set_text("室温过高 | 风扇降温")
        ui.status:set_style({ text_color = "#ffd28c" })
    else
        ui.status:set_text(string.format("正常 | 选择:%s(%s) 单击选/双击控", selected.label, mode))
        ui.status:set_style({ text_color = "#d7f5df" })
    end
    style_selection()
end

local function cleanup()
    if resources.motor then
        pcall(function() resources.motor:set_motor1(0) end)
    end
    if resources.strip then
        pcall(function() resources.strip:clear() end)
    end
    if resources.buzzer then
        pcall(function() resources.buzzer:stop() end)
        pcall(function() resources.buzzer:close() end)
    end
    if button then
        for _, name in ipairs({"confirm", "back"}) do
            local handle = resources[name]
            if handle then
                pcall(function() button.off(handle) end)
                pcall(function() button.close(handle) end)
            end
        end
    end
    if resources.audio then
        pcall(function() resources.audio.analyzer:close() end)
        pcall(function() resources.audio.input:close() end)
    end
    for _, name in ipairs({"light", "climate", "imu", "motor", "strip"}) do
        local handle = resources[name]
        if handle then pcall(function() handle:close() end) end
    end
    if lvgl_started then pcall(lvgl.deinit) end
end

local function run()
    ui = create_scene()
    open_hardware()
    setup_buttons()

    while true do
        state.loop_count = state.loop_count + 1
        if button then pcall(button.dispatch) end
        sample_light()
        sample_climate()
        sample_audio()
        sample_imu()
        decide_scene()
        apply_outputs()
        update_scene()
        lvgl.process_events(0)
        delay.delay_ms(CFG.loop_delay_ms)
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    error(err)
end
