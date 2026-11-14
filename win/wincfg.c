/* config/windows.ini: see wincfg.h. Unknown lines are dropped, bad values fall back to the defaults. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wincfg.h"
#include "win.h"

WinCfg g_win;

const char *const KB_NAMES[KB_COUNT] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+ (Start)", "- (menu)", "Left stick press", "Right stick press",
    "Left stick up", "Left stick down", "Left stick left", "Left stick right",
    "Right stick up", "Right stick down", "Right stick left", "Right stick right",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right",
};
static const char *const KB_KEYS_INI[KB_COUNT] = {
    "key_a", "key_b", "key_x", "key_y", "key_l", "key_r", "key_zl", "key_zr", "key_plus", "key_minus", "key_lstick", "key_rstick",
    "key_lup", "key_ldown", "key_lleft", "key_lright", "key_rup", "key_rdown", "key_rleft", "key_rright",
    "key_dup", "key_ddown", "key_dleft", "key_dright",
};
const int KB_DEFAULT[KB_COUNT][KB_KEYS] = {
    [KB_A] = {'X'}, [KB_B] = {'Z'}, [KB_X] = {'C'}, [KB_Y] = {'V'}, [KB_L] = {'Q'}, [KB_R] = {'E'},
    [KB_ZL] = {VK_SHIFT}, [KB_ZR] = {VK_SPACE}, [KB_PLUS] = {VK_RETURN}, [KB_MINUS] = {VK_ESCAPE, VK_TAB},
    [KB_LSTICK] = {'F'}, [KB_RSTICK] = {'R'},
    [KB_LUP] = {VK_UP, 'W'}, [KB_LDOWN] = {VK_DOWN, 'S'}, [KB_LLEFT] = {VK_LEFT, 'A'}, [KB_LRIGHT] = {VK_RIGHT, 'D'},
    [KB_RUP] = {'I'}, [KB_RDOWN] = {'K'}, [KB_RLEFT] = {'J'}, [KB_RRIGHT] = {'L'},
    [KB_DUP] = {VK_NUMPAD8}, [KB_DDOWN] = {VK_NUMPAD2}, [KB_DLEFT] = {VK_NUMPAD4}, [KB_DRIGHT] = {VK_NUMPAD6},
};

#define INI CFG_DIR "/windows.ini"

void wincfg_default_keys(void) { memcpy(g_win.keys, KB_DEFAULT, sizeof g_win.keys); }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void wincfg_load(void) {
    memset(&g_win, 0, sizeof g_win);
    g_win.launcher = true, g_win.win_w = 1280, g_win.win_h = 720, g_win.volume = 100, g_win.latency = 1;
    wincfg_default_keys();
    FILE *f = fopen(INI, "r");
    if (!f) return;
    char l[400];
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n")] = 0;
        char *v = strchr(l, '=');
        if (!v) continue;
        *v++ = 0;
        if (!strcmp(l, "launcher")) g_win.launcher = atoi(v) != 0;
        else if (!strcmp(l, "fullscreen")) g_win.fullscreen = atoi(v) != 0;
        else if (!strcmp(l, "auto_update")) g_win.auto_update = atoi(v) != 0;
        else if (!strcmp(l, "window")) {
            int w, h;
            if (sscanf(v, "%dx%d", &w, &h) == 2) g_win.win_w = clampi(w, 640, 7680), g_win.win_h = clampi(h, 480, 4320);
        } else if (!strcmp(l, "volume")) g_win.volume = clampi(atoi(v), 0, 100);
        else if (!strcmp(l, "latency")) g_win.latency = clampi(atoi(v), 0, 2);
        else if (!strcmp(l, "audio_device")) snprintf(g_win.audio_dev, sizeof g_win.audio_dev, "%s", v);
        else if (!strcmp(l, "disc")) snprintf(g_win.disc, sizeof g_win.disc, "%s", v);
        else if (l[0] == 'p' && l[1] >= '2' && l[1] <= '4' && !strcmp(l + 2, "_map")) {
            /* pN_map=a,b,...: WIN_MAPS hex masks; anything short or malformed is ignored */
            int pl = l[1] - '2', n = 0;
            uint64_t m[WIN_MAPS] = {0};
            for (char *s = v, *e; n < WIN_MAPS && *s; n++) {
                m[n] = strtoull(s, &e, 16);
                if (e == s) break;
                s = *e == ',' ? e + 1 : e;
            }
            if (n == WIN_MAPS) memcpy(g_win.pmap[pl], m, sizeof m), g_win.pmap_set[pl] = true;
        } else
            for (int i = 0; i < KB_COUNT; i++)
                if (!strcmp(l, KB_KEYS_INI[i])) {
                    int a = 0, b = 0;
                    sscanf(v, "%d,%d", &a, &b);
                    g_win.keys[i][0] = clampi(a, 0, 255), g_win.keys[i][1] = clampi(b, 0, 255);
                }
    }
    fclose(f);
}

void wincfg_save(void) {
    FILE *f = fopen(INI ".tmp", "w");
    if (!f) return;
    fprintf(f, "launcher=%d\nauto_update=%d\nfullscreen=%d\nwindow=%dx%d\nvolume=%d\nlatency=%d\naudio_device=%s\ndisc=%s\n", g_win.launcher,
            g_win.auto_update, g_win.fullscreen, g_win.win_w, g_win.win_h, g_win.volume, g_win.latency, g_win.audio_dev, g_win.disc);
    for (int i = 0; i < KB_COUNT; i++) fprintf(f, "%s=%d,%d\n", KB_KEYS_INI[i], g_win.keys[i][0], g_win.keys[i][1]);
    bool ok = !ferror(f);
    if (fclose(f) || !ok) { remove(INI ".tmp"); return; }
    MoveFileExA(INI ".tmp", INI, MOVEFILE_REPLACE_EXISTING);
}

void wincfg_key_name(int vk, char *out, int n) {
    out[0] = 0;
    if (vk <= 0 || n < 2) return;
    switch (vk) { /* names GetKeyNameText gets wrong or leaves out */
    case VK_SHIFT: snprintf(out, n, "Shift"); return;
    case VK_CONTROL: snprintf(out, n, "Ctrl"); return;
    case VK_MENU: snprintf(out, n, "Alt"); return;
    case VK_UP: snprintf(out, n, "Up"); return;
    case VK_DOWN: snprintf(out, n, "Down"); return;
    case VK_LEFT: snprintf(out, n, "Left"); return;
    case VK_RIGHT: snprintf(out, n, "Right"); return;
    }
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT) sc |= 0x100;
    if (!sc || !GetKeyNameTextA((LONG)(sc << 16), out, n)) snprintf(out, n, "Key %d", vk);
}
