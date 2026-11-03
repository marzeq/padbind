#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>
#include <hyprland/src/config/lua/LuaCaller.hpp>
#include <hyprland/src/config/lua/bindings/LuaBindingsInternal.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/protocols/InputCapture.hpp>
#include <hyprland/src/protocols/ShortcutsInhibit.hpp>

#include <SDL3/SDL.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Mask = uint64_t;
constexpr auto POLL_INTERVAL = std::chrono::milliseconds(8);
constexpr auto REPEAT_DELAY = std::chrono::milliseconds(400);
constexpr auto REPEAT_INTERVAL = std::chrono::milliseconds(60);
constexpr auto LONG_PRESS_DELAY = std::chrono::milliseconds(500);

struct Binding {
    Mask mask = 0;
    int luaRef = LUA_NOREF;
    bool release = false;
    bool repeating = false;
    bool longPress = false;
    bool locked = false;
    bool dontInhibit = false;
    bool submapUniversal = false;
    bool allowInputCapture = false;
    bool deviceInclusive = true;
    std::vector<std::string> devices;
    bool hasDeviceFilter = false;
    std::string submap;
};

struct BindingState {
    bool active = false;
    bool longFired = false;
    Clock::time_point nextFire{};
};

struct Gamepad {
    SDL_Gamepad* handle = nullptr;
    std::string name;
    Mask down = 0;
    std::vector<BindingState> states;
};

std::vector<Binding> bindings;
std::map<SDL_JoystickID, Gamepad> gamepads;
SP<CEventLoopTimer> pollTimer;
lua_State* callbackState = nullptr;
int stateTokenRef = LUA_NOREF;

void clearBindings() {
    bindings.clear();
    for (auto& [id, gamepad] : gamepads)
        gamepad.states.clear();
}

int onLuaStateClosed(lua_State*) {
    // The token is held in this state's registry, so this runs at lua_close.
    clearBindings();
    callbackState = nullptr;
    stateTokenRef = LUA_NOREF;
    return 0;
}

void watchLuaState(lua_State* L) {
    if (callbackState == L)
        return;
    // The previous state's finalizer has cleared the old references before
    // Hyprland can enter the replacement state.
    lua_newuserdata(L, 1);
    lua_newtable(L);
    lua_pushcfunction(L, onLuaStateClosed);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    stateTokenRef = luaL_ref(L, LUA_REGISTRYINDEX);
    callbackState = L;
}

constexpr Mask bit(SDL_GamepadButton button) {
    return Mask{1} << static_cast<unsigned>(button);
}

std::string normalize(std::string_view text) {
    std::string result;
    for (unsigned char c : text) {
        if (c == ' ' || c == '\t' || c == '-' || c == '_')
            continue;
        result += static_cast<char>(std::toupper(c));
    }
    return result;
}

SDL_GamepadButton buttonFromName(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, SDL_GamepadButton>, 56> NAMES{{
        {"A", SDL_GAMEPAD_BUTTON_SOUTH}, {"B", SDL_GAMEPAD_BUTTON_EAST}, {"X", SDL_GAMEPAD_BUTTON_WEST}, {"Y", SDL_GAMEPAD_BUTTON_NORTH},
        {"SOUTH", SDL_GAMEPAD_BUTTON_SOUTH}, {"EAST", SDL_GAMEPAD_BUTTON_EAST}, {"WEST", SDL_GAMEPAD_BUTTON_WEST}, {"NORTH", SDL_GAMEPAD_BUTTON_NORTH},
        {"CROSS", SDL_GAMEPAD_BUTTON_SOUTH}, {"CIRCLE", SDL_GAMEPAD_BUTTON_EAST}, {"SQUARE", SDL_GAMEPAD_BUTTON_WEST}, {"TRIANGLE", SDL_GAMEPAD_BUTTON_NORTH},
        {"BACK", SDL_GAMEPAD_BUTTON_BACK}, {"SELECT", SDL_GAMEPAD_BUTTON_BACK}, {"GUIDE", SDL_GAMEPAD_BUTTON_GUIDE}, {"HOME", SDL_GAMEPAD_BUTTON_GUIDE},
        {"LOGO", SDL_GAMEPAD_BUTTON_GUIDE}, {"XBOX", SDL_GAMEPAD_BUTTON_GUIDE}, {"PLAYSTATION", SDL_GAMEPAD_BUTTON_GUIDE}, {"PS", SDL_GAMEPAD_BUTTON_GUIDE},
        {"START", SDL_GAMEPAD_BUTTON_START}, {"LEFTSTICK", SDL_GAMEPAD_BUTTON_LEFT_STICK}, {"RIGHTSTICK", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {"L3", SDL_GAMEPAD_BUTTON_LEFT_STICK}, {"R3", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {"LB", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"RB", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {"LEFTSHOULDER", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"RIGHTSHOULDER", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {"DPADUP", SDL_GAMEPAD_BUTTON_DPAD_UP}, {"DPADDOWN", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {"DPADLEFT", SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"DPADRIGHT", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {"UP", SDL_GAMEPAD_BUTTON_DPAD_UP}, {"DOWN", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {"LEFT", SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"RIGHT", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {"MISC", SDL_GAMEPAD_BUTTON_MISC1}, {"MISC1", SDL_GAMEPAD_BUTTON_MISC1}, {"SHARE", SDL_GAMEPAD_BUTTON_MISC1}, {"CAPTURE", SDL_GAMEPAD_BUTTON_MISC1},
        {"MISC2", SDL_GAMEPAD_BUTTON_MISC2},
        {"MISC3", SDL_GAMEPAD_BUTTON_MISC3}, {"MISC4", SDL_GAMEPAD_BUTTON_MISC4}, {"MISC5", SDL_GAMEPAD_BUTTON_MISC5}, {"MISC6", SDL_GAMEPAD_BUTTON_MISC6},
        {"RIGHTPADDLE1", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1}, {"LEFTPADDLE1", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},
        {"RIGHTPADDLE2", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2}, {"LEFTPADDLE2", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2},
        {"TOUCHPAD", SDL_GAMEPAD_BUTTON_TOUCHPAD},
        {"LS", SDL_GAMEPAD_BUTTON_LEFT_STICK}, {"RS", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {"L1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"R1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {"MENU", SDL_GAMEPAD_BUTTON_START},
    }};
    const auto normalized = normalize(name);
    for (auto [alias, button] : NAMES)
        if (alias == normalized)
            return button;
    return SDL_GAMEPAD_BUTTON_INVALID;
}

Mask parseExpression(lua_State* L, std::string_view expression) {
    Mask mask = 0;
    size_t start = 0;
    do {
        const auto end = expression.find('+', start);
        const auto name = expression.substr(start, end == std::string_view::npos ? end : end - start);
        const auto button = buttonFromName(name);
        if (button == SDL_GAMEPAD_BUTTON_INVALID)
            luaL_error(L, "padbind.bind: unknown gamepad button '%.*s'", static_cast<int>(name.size()), name.data());
        if (mask & bit(button))
            luaL_error(L, "padbind.bind: duplicate button '%.*s'", static_cast<int>(name.size()), name.data());
        mask |= bit(button);
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    } while (true);
    return mask;
}

bool flag(lua_State* L, const char* name) {
    lua_getfield(L, 3, name);
    if (!lua_isnil(L, -1) && !lua_isboolean(L, -1))
        luaL_error(L, "padbind.bind: flag '%s' must be boolean", name);
    const bool value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

void parseDeviceFlag(lua_State* L, Binding& binding) {
    lua_getfield(L, 3, "device");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    luaL_checktype(L, -1, LUA_TTABLE);
    binding.hasDeviceFilter = true;
    lua_getfield(L, -1, "inclusive");
    if (!lua_isnil(L, -1)) {
        if (!lua_isboolean(L, -1))
            luaL_error(L, "padbind.bind: device.inclusive must be boolean");
        binding.deviceInclusive = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, -1, "list");
    if (!lua_istable(L, -1))
        luaL_error(L, "padbind.bind: device.list must be a table of SDL3 gamepad names");
    const auto count = lua_rawlen(L, -1);
    for (size_t i = 1; i <= count; ++i) {
        lua_rawgeti(L, -1, static_cast<lua_Integer>(i));
        if (lua_type(L, -1) != LUA_TSTRING)
            luaL_error(L, "padbind.bind: device.list entries must be strings");
        binding.devices.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 2); // list, device
}

int luaBind(lua_State* L) {
    size_t length = 0;
    const char* expression = luaL_checklstring(L, 1, &length);
    if (!lua_isnoneornil(L, 3) && !lua_istable(L, 3))
        return luaL_error(L, "padbind.bind: third argument must be a flags table");
    const Mask mask = parseExpression(L, {expression, length});
    Binding binding{.mask = mask};
    binding.submap = Config::Actions::state()->m_currentSubmap;
    if (lua_istable(L, 3)) {
        binding.release = flag(L, "release");
        binding.repeating = flag(L, "repeating");
        binding.longPress = flag(L, "long_press");
        binding.locked = flag(L, "locked");
        binding.dontInhibit = flag(L, "dont_inhibit");
        binding.submapUniversal = flag(L, "submap_universal");
        binding.allowInputCapture = flag(L, "allow_input_capture");
        parseDeviceFlag(L, binding);
        if (static_cast<int>(binding.release) + binding.repeating + binding.longPress > 1)
            return luaL_error(L, "padbind.bind: release, repeating and long_press are mutually exclusive");
        lua_pushnil(L);
        while (lua_next(L, 3)) {
            const char* key = lua_tostring(L, -2);
            if (!key || (std::string_view(key) != "release" && std::string_view(key) != "repeating" && std::string_view(key) != "long_press" &&
                         std::string_view(key) != "locked" && std::string_view(key) != "dont_inhibit" && std::string_view(key) != "submap_universal" &&
                         std::string_view(key) != "allow_input_capture" && std::string_view(key) != "device"))
                return luaL_error(L, "padbind.bind: unsupported flag '%s'", key ? key : "<non-string>");
            lua_pop(L, 1);
        }
    }
    auto* manager = Config::Lua::CConfigManager::fromLuaState(L);
    if (!manager)
        return luaL_error(L, "padbind.bind: Hyprland Lua manager unavailable");
    if (!Config::Lua::Bindings::Internal::pushDispatcherFunction(L, 2))
        return luaL_error(L, "padbind.bind: second argument must be an HL.Dispatcher or a Lua function");
    watchLuaState(L);
    binding.luaRef = luaL_ref(L, LUA_REGISTRYINDEX);
    bindings.push_back(binding);
    for (auto& [id, gamepad] : gamepads) {
        BindingState state;
        state.active = (gamepad.down & mask) == mask;
        state.nextFire = Clock::now() + (binding.longPress ? LONG_PRESS_DELAY : REPEAT_DELAY);
        gamepad.states.emplace_back(state);
    }
    return 0;
}

void invoke(const Binding& binding, const Gamepad& gamepad) {
    if (binding.hasDeviceFilter) {
        const bool listed = std::ranges::find(binding.devices, gamepad.name) != binding.devices.end();
        if (listed != binding.deviceInclusive)
            return;
    }
    if (!binding.locked && g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
        return;
    if (!binding.dontInhibit && PROTO::shortcutsInhibit && PROTO::shortcutsInhibit->isInhibited())
        return;
    if (!binding.allowInputCapture && PROTO::inputCapture && PROTO::inputCapture->isCaptured())
        return;
    if (!binding.submapUniversal && binding.submap != Config::Actions::state()->m_currentSubmap)
        return;
    if (auto manager = Config::Lua::mgr(); manager && manager->caller().state() == callbackState) {
        lua_rawgeti(callbackState, LUA_REGISTRYINDEX, binding.luaRef);
        // CLuaCaller reports callback errors to the config manager.
        (void)manager->caller().callPushed(0, Config::Lua::CLuaCaller::LUA_TIMEOUT_KEYBIND_CALLBACK_MS, "padbind callback");
    }
}

void updateBindings(Gamepad& gamepad, Clock::time_point now) {
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto& binding = bindings[i];
        auto& state = gamepad.states[i];
        const bool active = (gamepad.down & binding.mask) == binding.mask;
        if (active && !state.active) {
            state.active = true;
            state.longFired = false;
            state.nextFire = now + (binding.longPress ? LONG_PRESS_DELAY : REPEAT_DELAY);
            if (!binding.release && !binding.longPress)
                invoke(binding, gamepad);
        } else if (!active && state.active) {
            state.active = false;
            if (binding.release)
                invoke(binding, gamepad);
        } else if (active && binding.longPress && !state.longFired && now >= state.nextFire) {
            state.longFired = true;
            invoke(binding, gamepad);
        } else if (active && binding.repeating && now >= state.nextFire) {
            state.nextFire = now + REPEAT_INTERVAL;
            invoke(binding, gamepad);
        }
    }
}

void openGamepad(SDL_JoystickID id) {
    if (gamepads.contains(id) || !SDL_IsGamepad(id))
        return;
    SDL_Gamepad* handle = SDL_OpenGamepad(id);
    if (!handle)
        return;
    const char* name = SDL_GetGamepadName(handle);
    Gamepad gamepad{.handle = handle, .name = name ? name : ""};
    gamepad.states.resize(bindings.size());
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button)
        if (SDL_GetGamepadButton(handle, static_cast<SDL_GamepadButton>(button)))
            gamepad.down |= bit(static_cast<SDL_GamepadButton>(button));
    gamepads.emplace(id, std::move(gamepad));
}

void poll() {
    SDL_Event event;
    // Bound each pass so an event flood cannot monopolize the compositor loop.
    for (int i = 0; i < 64 && SDL_PollEvent(&event); ++i) {
        switch (event.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            openGamepad(event.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (auto it = gamepads.find(event.gdevice.which); it != gamepads.end()) {
                SDL_CloseGamepad(it->second.handle);
                gamepads.erase(it);
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (auto it = gamepads.find(event.gbutton.which); it != gamepads.end() && event.gbutton.button < SDL_GAMEPAD_BUTTON_COUNT) {
                const Mask button = bit(static_cast<SDL_GamepadButton>(event.gbutton.button));
                if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                    it->second.down |= button;
                else
                    it->second.down &= ~button;
                updateBindings(it->second, Clock::now());
            }
            break;
        default:
            break;
        }
    }
    const auto now = Clock::now();
    for (auto& [id, gamepad] : gamepads)
        updateBindings(gamepad, now);
}

void shutdown() {
    if (callbackState && stateTokenRef != LUA_NOREF) {
        for (const auto& binding : bindings)
            luaL_unref(callbackState, LUA_REGISTRYINDEX, binding.luaRef);
        lua_rawgeti(callbackState, LUA_REGISTRYINDEX, stateTokenRef);
        lua_pushnil(callbackState);
        lua_setmetatable(callbackState, -2);
        lua_pop(callbackState, 1);
        luaL_unref(callbackState, LUA_REGISTRYINDEX, stateTokenRef);
        callbackState = nullptr;
        stateTokenRef = LUA_NOREF;
    }
    if (pollTimer) {
        pollTimer->cancel();
        g_pEventLoopManager->removeTimer(pollTimer);
        pollTimer.reset();
    }
    for (auto& [id, gamepad] : gamepads)
        SDL_CloseGamepad(gamepad.handle);
    gamepads.clear();
    clearBindings();
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    if (std::string(__hyprland_api_get_hash()) != __hyprland_api_get_client_hash())
        throw std::runtime_error("padbind: Hyprland headers do not match the running compositor");
    if (!HyprlandAPI::addLuaFunction(handle, "padbind", "bind", luaBind))
        throw std::runtime_error("padbind: native Lua config is required");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        HyprlandAPI::removeLuaFunction(handle, "padbind", "bind");
        throw std::runtime_error(std::string("padbind: SDL gamepad initialization failed: ") + SDL_GetError());
    }
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i)
            openGamepad(ids[i]);
        SDL_free(ids);
    }
    pollTimer = makeShared<CEventLoopTimer>(POLL_INTERVAL, [](SP<CEventLoopTimer> self, void*) {
        poll();
        self->updateTimeout(POLL_INTERVAL);
    }, nullptr);
    g_pEventLoopManager->addTimer(pollTimer);
    HyprlandAPI::reloadConfig();
    return {"padbind", "SDL3 gamepad bindings for native Lua configuration", "padbind contributors", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    shutdown();
}
