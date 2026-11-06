/* Controllers through SDL2 (pad.h). SDL runs without its video part: the window is main_win.c's,
 * so controller events are taken whatever window has the focus (main_win.c drops them while the
 * game's window is in the background). */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

#include "nx_compat.h"
#include "pad.h"
#include "win.h"
#include "../runtime/rt.h"

Pad g_pads[PAD_SLOTS];
static SDL_GameController *s_ctl[PAD_SLOTS];
static SDL_JoystickID s_id[PAD_SLOTS];
static uint16_t s_rumble[PAD_SLOTS];
static uint32_t s_rumble_at[PAD_SLOTS];
static bool s_ok;

bool pad_init(void) {
    if (s_ok) return true;
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        rt_log("pad: SDL_Init failed: %s (keyboard only)", SDL_GetError());
        return false;
    }
    s_ok = true;
    /* more controllers: SDL's community mapping file, when the player puts one in config/ */
    int n = SDL_GameControllerAddMappingsFromFile(CFG_DIR "/gamecontrollerdb.txt");
    if (n > 0) rt_log("pad: %d mappings from " CFG_DIR "/gamecontrollerdb.txt", n);
    SDL_version v;
    SDL_GetVersion(&v);
    rt_log("pad: SDL %d.%d.%d game controllers", v.major, v.minor, v.patch);
    return true;
}

static int slot_of(SDL_JoystickID id) {
    for (int i = 0; i < PAD_SLOTS; i++)
        if (s_ctl[i] && s_id[i] == id) return i;
    return -1;
}

static void open_device(int index) {
    if (!SDL_IsGameController(index)) {
        rt_log("pad: \"%s\" has no controller mapping (add one to " CFG_DIR "/gamecontrollerdb.txt)", SDL_JoystickNameForIndex(index));
        return;
    }
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
    if (slot_of(id) >= 0) return;
    int s = 0;
    while (s < PAD_SLOTS && s_ctl[s]) s++;
    if (s == PAD_SLOTS) return;
    SDL_GameController *c = SDL_GameControllerOpen(index);
    if (!c) return;
    s_ctl[s] = c, s_id[s] = id;
    Pad *p = &g_pads[s];
    memset(p, 0, sizeof *p);
    p->on = true;
    /* the first player number no other controller has */
    for (p->player = 0; p->player < PAD_SLOTS; p->player++) {
        bool taken = false;
        for (int i = 0; i < PAD_SLOTS; i++) taken |= i != s && g_pads[i].on && g_pads[i].player == p->player;
        if (!taken) break;
    }
    const char *name = SDL_GameControllerName(c);
    snprintf(p->name, sizeof p->name, "%s", name ? name : "Controller");
    /* the official adapter (WUP-028) and SDL's name for its ports */
    p->gc = (SDL_GameControllerGetVendor(c) == 0x057E && SDL_GameControllerGetProduct(c) == 0x0337) || strstr(p->name, "GameCube");
    /* A / B / X / Y are SDL's labels (the hint above): Xbox and PlayStation pads carry them in
     * a different place than a Nintendo pad does. SDL's type, not the maker: third-party Switch pads
     * (PowerA, Hori, 8BitDo in Switch mode) have Nintendo's layout under their own vendor ID. */
    SDL_GameControllerType type = SDL_GameControllerGetType(c);
    bool nin = SDL_GameControllerGetVendor(c) == 0x057E || type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO ||
               type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT || type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT ||
               type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    p->xface = !p->gc && !nin;
    rt_log("pad: slot %d: %s%s, player %d, type %d, %s layout", s, p->name, p->gc ? " (GameCube layout)" : "", p->player + 1,
           (int)type, p->xface ? "Xbox" : "Nintendo");
}

static void close_slot(int s) {
    rt_log("pad: slot %d: %s disconnected", s, g_pads[s].name);
    SDL_GameControllerClose(s_ctl[s]);
    s_ctl[s] = NULL;
    memset(&g_pads[s], 0, sizeof g_pads[s]);
    s_rumble[s] = 0;
}

static int32_t axis(SDL_GameController *c, SDL_GameControllerAxis a, bool invert) {
    int v = SDL_GameControllerGetAxis(c, a);
    if (invert) v = -v - 1;
    if (v > -7849 && v < 7849) return 0; /* XInput's left-stick dead zone */
    return v < -32767 ? -32767 : v > 32767 ? 32767 : v;
}

static void stick_bits(uint64_t *b, int32_t x, int32_t y, bool left) {
    const int32_t T = 16000;
    if (x < -T) *b |= left ? HidNpadButton_StickLLeft : HidNpadButton_StickRLeft;
    if (x > T) *b |= left ? HidNpadButton_StickLRight : HidNpadButton_StickRRight;
    if (y > T) *b |= left ? HidNpadButton_StickLUp : HidNpadButton_StickRUp;
    if (y < -T) *b |= left ? HidNpadButton_StickLDown : HidNpadButton_StickRDown;
}

void pad_poll(void) {
    if (!s_ok) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_CONTROLLERDEVICEADDED) open_device(e.cdevice.which);
        else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
            int s = slot_of(e.cdevice.which);
            if (s >= 0) close_slot(s);
        }
    }
    static const struct { SDL_GameControllerButton sb; uint64_t bit; } BTN[] = {
        {SDL_CONTROLLER_BUTTON_A, HidNpadButton_A}, {SDL_CONTROLLER_BUTTON_B, HidNpadButton_B},
        {SDL_CONTROLLER_BUTTON_X, HidNpadButton_X}, {SDL_CONTROLLER_BUTTON_Y, HidNpadButton_Y},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, HidNpadButton_L}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, HidNpadButton_R},
        {SDL_CONTROLLER_BUTTON_START, HidNpadButton_Plus}, {SDL_CONTROLLER_BUTTON_BACK, HidNpadButton_Minus},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, HidNpadButton_StickL}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, HidNpadButton_StickR},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, HidNpadButton_Up}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, HidNpadButton_Down},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, HidNpadButton_Left}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, HidNpadButton_Right},
        {SDL_CONTROLLER_BUTTON_GUIDE, HidNpadButton_Minus}, /* PS / Home / Xbox button: the menu as well */
    };
    uint32_t now = SDL_GetTicks();
    for (int s = 0; s < PAD_SLOTS; s++) {
        Pad *p = &g_pads[s];
        p->prev = p->b;
        SDL_GameController *c = s_ctl[s];
        if (!c) continue;
        uint64_t b = 0;
        for (size_t i = 0; i < sizeof BTN / sizeof *BTN; i++)
            if (SDL_GameControllerGetButton(c, BTN[i].sb)) b |= BTN[i].bit;
        int lt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT), rt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        p->lt = (uint8_t)(lt <= 0 ? 0 : lt * 255 / 32767), p->rt = (uint8_t)(rt <= 0 ? 0 : rt * 255 / 32767);
        if (p->gc) {
            /* GameCube: Z (SDL's right shoulder) is the Switch's ZR for it, L / R are the analog triggers */
            if (b & HidNpadButton_R) b = (b & ~HidNpadButton_R) | HidNpadButton_ZR;
            b &= ~HidNpadButton_L;
            if (p->lt > 200) b |= HidNpadButton_L;
            if (p->rt > 200) b |= HidNpadButton_R;
        } else {
            if (p->lt > 60) b |= HidNpadButton_ZL;
            if (p->rt > 60) b |= HidNpadButton_ZR;
        }
        p->lx = axis(c, SDL_CONTROLLER_AXIS_LEFTX, false), p->ly = axis(c, SDL_CONTROLLER_AXIS_LEFTY, true);
        p->rx = axis(c, SDL_CONTROLLER_AXIS_RIGHTX, false), p->ry = axis(c, SDL_CONTROLLER_AXIS_RIGHTY, true);
        stick_bits(&b, p->lx, p->ly, true);
        stick_bits(&b, p->rx, p->ry, false);
        p->b = b;
        /* rumble runs for a set time: renewed while it stays on */
        if (s_rumble[s] && now - s_rumble_at[s] > 500) {
            SDL_GameControllerRumble(c, s_rumble[s], s_rumble[s], 1000);
            s_rumble_at[s] = now;
        }
    }
}

void pad_rumble(int s, uint16_t strength) {
    if (s < 0 || s >= PAD_SLOTS || !s_ctl[s] || s_rumble[s] == strength) return;
    s_rumble[s] = strength;
    s_rumble_at[s] = SDL_GetTicks();
    SDL_GameControllerRumble(s_ctl[s], strength, strength, strength ? 1000 : 0);
}

void pad_quit(void) {
    if (!s_ok) return;
    for (int s = 0; s < PAD_SLOTS; s++)
        if (s_ctl[s]) pad_rumble(s, 0), SDL_GameControllerClose(s_ctl[s]), s_ctl[s] = NULL;
    memset(g_pads, 0, sizeof g_pads);
    SDL_Quit();
    s_ok = false;
}
