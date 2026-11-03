# padbind

`padbind` is a native Hyprland plugin that allows you to bind gamepad buttons to Hyprland dispatchers. It uses SDL3 to read gamepad input, so it supports any controller that SDL3 supports.

## Build and load

### HyprPM

```sh
hyprpm add https://github.com/marzeq/padbind.git
hyprpm enable padbind
hyprpm reload
```

Run `hyprpm update` after updating Hyprland to rebuild the plugin against the new headers. Building requires CMake, a C++23 compiler, pkg-config, and development packages for SDL3, Lua, and the libraries listed in `CMakeLists.txt`.

### Manual build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
hyprctl plugin load "$(pwd)/build/padbind.so"
```

For persistent loading, add the absolute path to your native Lua config:

```lua
hl.plugin.load("/absolute/path/to/padbind.so")
```

The plugin registers `hl.plugin.padbind.bind` which has an analogous API to `hl.bind`. It needs a native `hyprland.lua` config, it cannot register into a legacy `hyprland.conf` session.

## Bindings

```lua
hl.plugin.padbind.bind("MENU + LB", hl.dsp.focus({ workspace = "1" }))
hl.plugin.padbind.bind("MENU + RB", hl.dsp.focus({ workspace = "2" }))

hl.plugin.padbind.bind("CAPTURE", function()
  hl.dispatch(hl.dsp.exec_cmd("python ./scripts/capture_clip.py"))
end)
```

The second argument accepts an `HL.Dispatcher` from `hl.dsp` or a Lua function. Use a function when the binding needs to run multiple actions or custom Lua code.

Each connected controller has independent state; buttons from different controllers cannot combine into one chord.

| SDL3 button | Supported spellings | Comment |
| --- | --- | --- |
| South face button | `SOUTH`, `A`, `CROSS` | |
| East face button | `EAST`, `B`, `CIRCLE` | |
| West face button | `WEST`, `X`, `SQUARE` | |
| North face button | `NORTH`, `Y`, `TRIANGLE` | |
| Select | `SELECT`, `BACK` | To the left of the logo button. |
| Logo | `LOGO`, `XBOX`, `PLAYSTATION`, `PS`, `GUIDE`, `HOME`  | Often a logo button like the XBOX or PlayStation button. |
| Menu | `MENU`, `START`  | To the right of the logo button. |
| Left stick button | `LEFT_STICK`, `LS`, `L3` | When the stick is pressed, not when it is moved. |
| Right stick button | `RIGHT_STICK`, `RS`, `R3` | Same as above. |
| Left shoulder | `LEFT_SHOULDER`, `LB`, `L1` | Not the trigger. |
| Right shoulder | `RIGHT_SHOULDER`, `RB`, `R1` | Same as above. |
| D-pad up | `DPAD_UP`, `UP` | |
| D-pad down | `DPAD_DOWN`, `DOWN` | |
| D-pad left | `DPAD_LEFT`, `LEFT` | |
| D-pad right | `DPAD_RIGHT`, `RIGHT` | |
| Misc 1 | `MISC1`, `MISC`, `SHARE`, `CAPTURE` | Extra button such as Share, Capture, microphone, or Quick Access. If your controller has a nonstandard extra button such as these, it may be one of these misc buttons. |
| Misc 2 | `MISC2` | Another extra button. Again, varies by controller. |
| Misc 3 | `MISC3` | |
| Misc 4 | `MISC4` | |
| Misc 5 | `MISC5` | |
| Misc 6 | `MISC6` | |
| Left paddle 1 | `LEFT_PADDLE1` | Even if your controller has paddles, they may not always be passed through to the driver. SDL3 may not see them. They may also be mapped to misc buttons. |
| Left paddle 2 | `LEFT_PADDLE2` | |
| Right paddle 1 | `RIGHT_PADDLE1` | |
| Right paddle 2 | `RIGHT_PADDLE2` | |
| Touchpad button | `TOUCHPAD` | For PlayStation controllers with a clickable touchpad. |

Names are case insensitive; spaces, underscores and hyphens inside a name are ignored. Face button names refer to positions, independent of the controller's printed labels. Axis and trigger values are not buttons.

### Flags

The optional third argument accepts these flags:

```lua
hl.plugin.padbind.bind("GUIDE + A", callback, { release = true })
hl.plugin.padbind.bind("GUIDE + B", callback, { repeating = true })
hl.plugin.padbind.bind("GUIDE + X", callback, { long_press = true })
hl.plugin.padbind.bind("GUIDE + Y", callback, {
    device = { list = { "Your SDL3 gamepad name" } }
})
```

| Flag | Effect |
| --- | --- |
| `release` | Run when a previously complete chord breaks, instead of when it becomes complete. |
| `repeating` | Run on activation, then after 400 ms and every 60 ms while held. |
| `long_press` | Run once after 500 ms of continuous hold. |
| `locked` | Allow the binding while the session is locked. |
| `dont_inhibit` | Allow the binding while keyboard shortcuts are inhibited. |
| `submap_universal` | Allow the binding in any submap. By default, it works only in the submap where it was created. |
| `allow_input_capture` | Allow the binding while input capture is active. |
| `device` | Limit the binding to SDL3 gamepads in `device.list`, or exclude them with `device.inclusive = false`. |

`release`, `repeating`, and `long_press` are mutually exclusive. Without them, a callback runs once when the chord becomes complete.

The `device` flag uses Hyprland's table shape: `device = { inclusive = true, list = { "name1", "name2" } }`. `inclusive` defaults to `true`; set it to `false` to allow every gamepad except those in the list.

#### Finding a controller name

Run `cat /sys/class/input/js*/device/name`. This will show the SDL3 name of each connected controller, each on a separate line. Use the name exactly as it appears, including case and spaces. 
