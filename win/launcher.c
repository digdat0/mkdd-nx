/* The launcher: a settings window shown before the game (wincfg.h launcher; Shift held at start or
 * --launcher shows it anyway, --play skips it). It edits the game's settings.ini (g_set, menu.c:
 * the same settings as the in-game - menu) and windows.ini (g_win: window, sound, keyboard), and
 * installs mods. Win32 controls in a dark theme: buttons, toggles and the page list are owner-drawn,
 * combo boxes and lists use the system's dark control themes, the window paints the header and the
 * page panel. No resources besides the manifest (mkdd.rc). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nx_compat.h"
#include "pad.h"
#include "win.h"
#include "wincfg.h"
#include "../runtime/rt.h"
#include "../runtime/table.h"
#include "../switch/menu.h"
#include "../switch/update.h"

enum { P_GAME, P_GFX, P_AUDIO, P_CTRL, P_KEYS, P_MODS, P_COUNT };
static const char *const PAGE_NAMES[P_COUNT] = {"Game", "Graphics", "Audio", "Controller", "Keyboard", "Mods"};
/* Segoe MDL2 Assets glyphs: flag, monitor, volume, game pad, keyboard, puzzle piece */
static const wchar_t PAGE_ICONS[P_COUNT] = {0xE7C1, 0xE7F4, 0xE767, 0xE7FC, 0xE765, 0xEA86};

enum { /* control ids */
    ID_PLAY = 100, ID_QUIT, ID_BROWSE, ID_OPEN_DIR, ID_OPEN_MODS, ID_INSTALL, ID_MAP_DEFAULT, ID_KEYS_DEFAULT,
    ID_VOLUME, ID_DISC, ID_PAD_VIEW, ID_PAD_PLAYER,
    ID_MAP0 = 200,  /* + row * 2 + column */
    ID_KEY0 = 300,  /* + row * KB_KEYS + column */
    ID_TOG0 = 400,  /* + toggle number */
    ID_NAV0 = 500,  /* + page */
};

/* what a control is (GWLP_USERDATA): its kind, whether it sits on the sidebar, a toggle's state */
enum { K_TEXT, K_HEAD, K_NOTE, K_BTN, K_PLAY, K_NAV, K_TOGGLE, K_KEY, K_PADVIEW, K_KIND = 0xff, F_SIDE = 0x100, F_ON = 0x200, F_LIT = 0x400 };

/* the colors */
#define C_BG RGB(17, 18, 23)
#define C_PANEL RGB(29, 31, 39)
#define C_FIELD RGB(21, 22, 28)
#define C_TEXT RGB(236, 237, 242)
#define C_DIM RGB(146, 150, 165)
#define C_GOLD RGB(255, 198, 41)
#define C_RED RGB(228, 32, 48)
#define C_RED_HOT RGB(246, 58, 72)
#define C_RED_DOWN RGB(178, 20, 34)
#define C_BTN RGB(48, 51, 63)
#define C_BTN_HOT RGB(62, 66, 81)
#define C_BTN_DOWN RGB(38, 40, 50)
#define C_OFF RGB(72, 76, 92)

/* the layout (96 dpi pixels; S() scales): client size, header, sidebar, page panel */
enum { CW = 880, CH = 584, HDR = 72, CHECK = 6, SIDE_X = 12, SIDE_W = 160, PANEL_X = 184, PANEL_Y = HDR + 2 * CHECK + 12 };

static HWND s_wnd;
static HFONT s_font, s_bold, s_title, s_big, s_nav, s_icons;
static HBRUSH s_br_bg, s_br_panel, s_br_field;
static int s_dpi = 96, s_page, s_ntog;
static HWND s_hot; /* the owner-drawn control under the mouse */
static bool s_play, s_done;
static struct { HWND h; int page; } s_ctl[300];
static int s_nctl;

/* the controls whose values are read back on PLAY */
static HWND c_disc, c_wide, c_fps, c_log, c_lan, c_peer, c_show;
static HWND c_winsize, c_full, c_res, c_aniso, c_hd, c_border, c_fov43, c_fov169;
static HWND c_dev, c_vol, c_vol_txt, c_lat, c_music;
static HWND c_pads, c_rumble, c_map[MAP_COUNT][2], c_pad_sel, c_pad_player;
static HWND c_key[KB_COUNT][KB_KEYS];
static HWND c_lang;           /* Mods: the language (MODS > LANGUAGE), English + the ones in mods\lang */
static int s_lang_sel[LANG_COUNT];
static HWND c_mod, c_mods, c_chars, c_karts, c_progress, c_status;
static HWND c_nav[P_COUNT];
static int s_capture = -1;         /* the key button waiting for a key: row * KB_KEYS + column */
static bool s_installing;          /* the progress bar is up */
static bool s_disc_locked;         /* a mod install opened this disc image: it is the one the game plays */
static char s_discs[16][260];      /* the disc combo's paths (0: automatic) */
static int s_ndiscs;
/* The driving maps, one per player (settings.ini, shared with the Switch): players 2-4 have their own
 * once edited here or in their - menu (s_map_own, g_set.pl[].own), else drive with player 1's. The
 * page shows and edits the shown controller's player's (s_map_pl). */
static uint64_t s_maps[4][MAP_COUNT];
static bool s_map_own[4];
static int s_map_pl;
static HWND c_map_head;
static uint64_t *map_cur(void) { return s_maps[s_map_own[s_map_pl] ? s_map_pl : 0]; }
/* about to change the shown player's map: a player still on player 1's gets its own copy first */
static uint64_t *map_edit(void) {
    if (!s_map_own[s_map_pl]) memcpy(s_maps[s_map_pl], s_maps[0], sizeof s_maps[0]), s_map_own[s_map_pl] = true;
    return s_maps[s_map_pl];
}

static int S(int v) { return MulDiv(v, s_dpi, 96); }
static LONG_PTR info(HWND c) { return c ? GetWindowLongPtrA(c, GWLP_USERDATA) : 0; }

static HWND add(int page, const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | style, S(x), S(y), S(w), S(h), s_wnd, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    if (!c) return NULL;
    SendMessageA(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    SetWindowLongPtrA(c, GWLP_USERDATA, page < 0 ? F_SIDE : 0);
    if (s_nctl < (int)(sizeof s_ctl / sizeof *s_ctl)) s_ctl[s_nctl].h = c, s_ctl[s_nctl++].page = page;
    if (page < 0 || page == P_GAME) ShowWindow(c, SW_SHOW);
    return c;
}
static HWND kind(HWND c, int k) {
    if (c) SetWindowLongPtrA(c, GWLP_USERDATA, info(c) | k);
    return c;
}
static HWND label(int page, const char *t, int x, int y, int w) { return add(page, "STATIC", t, SS_NOPREFIX, x, y + 3, w, 20, 0); }
static HWND note(int page, const char *t, int x, int y, int w, int h) { return kind(add(page, "STATIC", t, SS_NOPREFIX, x, y, w, h, 0), K_NOTE); }
static HWND heading(int page, const char *t, int x, int y) {
    HWND h = kind(add(page, "STATIC", t, SS_NOPREFIX, x, y, 640, 22, 0), K_HEAD);
    if (h) SendMessageA(h, WM_SETFONT, (WPARAM)s_bold, TRUE);
    return h;
}
static HWND toggle(int page, const char *t, bool on, int x, int y, int w) {
    HWND c = kind(add(page, "BUTTON", t, WS_TABSTOP | BS_OWNERDRAW, x, y, w, 24, ID_TOG0 + s_ntog++), K_TOGGLE);
    if (c && on) SetWindowLongPtrA(c, GWLP_USERDATA, info(c) | F_ON);
    return c;
}
static bool checked(HWND c) { return (info(c) & F_ON) != 0; }
static HWND combo(int page, int x, int y, int w, int id) {
    HWND c = add(page, "COMBOBOX", "", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, x, y, w, 300, id);
    if (c) SetWindowTheme(c, L"DarkMode_CFD", NULL);
    return c;
}
static void combo_add(HWND c, const char *t) { SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)t); }
static void combo_set(HWND c, int i) { SendMessageA(c, CB_SETCURSEL, (WPARAM)i, 0); }
static int combo_get(HWND c) { int i = (int)SendMessageA(c, CB_GETCURSEL, 0, 0); return i < 0 ? 0 : i; }
static HWND combo_list(int page, int x, int y, int w, const char *const *items, int n, int sel) {
    HWND c = combo(page, x, y, w, 0);
    for (int i = 0; i < n; i++) combo_add(c, items[i]);
    combo_set(c, sel < 0 || sel >= n ? 0 : sel);
    return c;
}
static HWND button(int page, const char *t, int x, int y, int w, int id) {
    return kind(add(page, "BUTTON", t, WS_TABSTOP | BS_OWNERDRAW, x, y, w, 28, id), K_BTN);
}

static void show_page(int page) {
    if (page < 0 || page >= P_COUNT) return;
    s_page = page;
    for (int i = 0; i < s_nctl; i++)
        if (s_ctl[i].page >= 0) ShowWindow(s_ctl[i].h, s_ctl[i].page == page ? SW_SHOW : SW_HIDE);
    if (!s_installing) ShowWindow(c_progress, SW_HIDE);
    for (int i = 0; i < P_COUNT; i++) InvalidateRect(c_nav[i], NULL, FALSE);
    s_capture = -1;
}

/* ---- drawing ---- */
static void fill(HDC dc, RECT r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}
static void round_rect(HDC dc, RECT r, int rad, COLORREF c) {
    SetDCBrushColor(dc, c), SetDCPenColor(dc, c);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH)), op = SelectObject(dc, GetStockObject(DC_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob), SelectObject(dc, op);
}
static void outline(HDC dc, RECT r, int rad, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, S(2) > 1 ? S(2) : 1, c);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH)), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob), SelectObject(dc, op);
    DeleteObject(p);
}
static void circle(HDC dc, int x, int y, int d, COLORREF c) {
    SetDCBrushColor(dc, c), SetDCPenColor(dc, c);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH)), op = SelectObject(dc, GetStockObject(DC_PEN));
    Ellipse(dc, x, y, x + d, y + d);
    SelectObject(dc, ob), SelectObject(dc, op);
}
static void text(HDC dc, const char *t, RECT r, HFONT f, COLORREF c, UINT fmt) {
    SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextA(dc, t, -1, &r, fmt | DT_NOPREFIX);
}
static void glyph(HDC dc, wchar_t g, RECT r, COLORREF c, UINT fmt) {
    SelectObject(dc, s_icons);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, &g, 1, &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}
static RECT inset(RECT r, int d) { return (RECT){r.left + d, r.top + d, r.right - d, r.bottom - d}; }

static void draw_item(const DRAWITEMSTRUCT *di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    LONG_PTR in = info(di->hwndItem);
    int k = (int)(in & K_KIND);
    bool down = di->itemState & ODS_SELECTED, off = di->itemState & ODS_DISABLED, hot = di->hwndItem == s_hot && !off;
    bool focus = (di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT);
    char t[128];
    GetWindowTextA(di->hwndItem, t, sizeof t);
    fill(dc, r, in & F_SIDE ? C_BG : C_PANEL);
    int rad = S(10);
    if (k == K_NAV) {
        int page = (int)di->CtlID - ID_NAV0;
        bool sel = page == s_page;
        if (sel || hot) round_rect(dc, r, rad, sel ? C_PANEL : RGB(26, 28, 35));
        if (sel) round_rect(dc, (RECT){r.left, r.top + S(9), r.left + S(4), r.bottom - S(9)}, S(4), C_RED);
        COLORREF c = sel || hot ? C_TEXT : C_DIM;
        if (page >= 0 && page < P_COUNT) glyph(dc, PAGE_ICONS[page], (RECT){r.left + S(16), r.top, r.left + S(40), r.bottom}, sel ? C_GOLD : c, DT_CENTER);
        text(dc, t, (RECT){r.left + S(50), r.top, r.right - S(6), r.bottom}, s_nav, c, DT_SINGLELINE | DT_VCENTER);
        if (focus) outline(dc, inset(r, S(1)), rad, C_GOLD);
    } else if (k == K_PLAY) {
        round_rect(dc, r, S(14), off ? C_BTN : down ? C_RED_DOWN : hot ? C_RED_HOT : C_RED);
        SIZE sz;
        SelectObject(dc, s_big);
        GetTextExtentPoint32A(dc, t, (int)strlen(t), &sz);
        int gw = S(22), x = (r.left + r.right - (gw + S(8) + sz.cx)) / 2;
        glyph(dc, 0xE768, (RECT){x, r.top, x + gw, r.bottom}, off ? C_DIM : RGB(255, 255, 255), DT_LEFT);
        text(dc, t, (RECT){x + gw + S(8), r.top, r.right, r.bottom}, s_big, off ? C_DIM : RGB(255, 255, 255), DT_SINGLELINE | DT_VCENTER);
        if (focus) outline(dc, inset(r, S(3)), S(10), RGB(255, 225, 140));
    } else if (k == K_TOGGLE) {
        bool on = in & F_ON;
        int h = S(18), w = S(34), cy = (r.top + r.bottom) / 2;
        RECT pill = {r.left + S(1), cy - h / 2, r.left + S(1) + w, cy - h / 2 + h};
        round_rect(dc, pill, h, off ? C_BTN : on ? (hot ? C_RED_HOT : C_RED) : (hot ? RGB(88, 92, 110) : C_OFF));
        int kd = h - S(6), kx = on ? pill.right - S(3) - kd : pill.left + S(3);
        circle(dc, kx, cy - kd / 2, kd, off ? C_DIM : RGB(255, 255, 255));
        if (focus) outline(dc, inset(pill, -S(3)), h + S(6), C_GOLD);
        text(dc, t, (RECT){pill.right + S(10), r.top, r.right, r.bottom}, s_font, off ? C_DIM : C_TEXT, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    } else { /* K_BTN, K_KEY */
        bool wait = k == K_KEY && s_capture == (int)di->CtlID - ID_KEY0;
        COLORREF bg = wait ? C_RED : down ? C_BTN_DOWN : hot ? C_BTN_HOT : C_BTN;
        round_rect(dc, r, S(8), bg);
        if (focus && !wait) outline(dc, inset(r, S(1)), S(8), C_GOLD);
        text(dc, t, inset(r, S(4)), s_font, off ? C_DIM : C_TEXT, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
    }
}

/* the window itself: header with the title and a checkered strip, the page panel */
static void paint(HWND w) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(w, &ps);
    RECT cr;
    GetClientRect(w, &cr);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bm = CreateCompatibleBitmap(wdc, cr.right, cr.bottom);
    HGDIOBJ obm = SelectObject(dc, bm);
    fill(dc, cr, C_BG);
    int hh = S(HDR), sq = S(CHECK);
    for (int x = 0; x < cr.right; x += 2) { /* red to deep red */
        int a = cr.right > 0 ? x * 256 / cr.right : 0;
        fill(dc, (RECT){x, 0, x + 2, hh}, RGB(214 - a * 104 / 256, 20 - a * 12 / 256, 40 - a * 12 / 256));
    }
    for (int row = 0; row < 2; row++)
        for (int col = 0; col * sq < cr.right; col++)
            fill(dc, (RECT){col * sq, hh + row * sq, col * sq + sq, hh + row * sq + sq}, (row + col) & 1 ? RGB(18, 18, 20) : RGB(240, 240, 240));
    SIZE sz = {0};
    SelectObject(dc, s_title);
    GetTextExtentPoint32A(dc, "MKDD-NX", 7, &sz);
    text(dc, "MKDD-NX", (RECT){S(24) + S(2), S(2), cr.right, hh + S(2)}, s_title, RGB(110, 8, 22), DT_SINGLELINE | DT_VCENTER);
    text(dc, "MKDD-NX", (RECT){S(24), 0, cr.right, hh}, s_title, RGB(255, 255, 255), DT_SINGLELINE | DT_VCENTER);
    text(dc, "Mario Kart: Double Dash!!", (RECT){S(24) + sz.cx + S(16), 0, cr.right, hh}, s_nav, RGB(255, 222, 222), DT_SINGLELINE | DT_VCENTER);
    char ver[64];
    snprintf(ver, sizeof ver, "version %s for Windows", update_version());
    text(dc, ver, (RECT){0, 0, cr.right - S(24), hh}, s_font, RGB(255, 200, 200), DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    round_rect(dc, (RECT){S(PANEL_X), S(PANEL_Y), cr.right - S(12), cr.bottom - S(12)}, S(16), C_PANEL);
    BitBlt(wdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obm);
    DeleteObject(bm), DeleteDC(dc);
    EndPaint(w, &ps);
}

/* the volume slider: a red bar and a white knob */
static LRESULT slider_draw(NMCUSTOMDRAW *cd) {
    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    RECT th;
    SendMessageA(c_vol, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
    int cx = (th.left + th.right) / 2, cy = (th.top + th.bottom) / 2;
    if (cd->dwItemSpec == TBCD_CHANNEL) {
        RECT ch = cd->rc;
        int y = (ch.top + ch.bottom) / 2;
        round_rect(cd->hdc, (RECT){ch.left, y - S(3), ch.right, y + S(3)}, S(6), C_OFF);
        round_rect(cd->hdc, (RECT){ch.left, y - S(3), cx, y + S(3)}, S(6), C_RED);
        return CDRF_SKIPDEFAULT;
    }
    if (cd->dwItemSpec == TBCD_THUMB) {
        int d = S(16);
        circle(cd->hdc, cx - d / 2, cy - d / 2, d, RGB(255, 255, 255));
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_SKIPDEFAULT; /* no tics */
}

static void set_hot(HWND h) {
    int k = (int)(info(h) & K_KIND);
    if (h && (k < K_BTN || GetParent(h) != s_wnd)) h = NULL;
    if (h == s_hot) return;
    HWND old = s_hot;
    s_hot = h;
    if (old) InvalidateRect(old, NULL, FALSE);
    if (h) InvalidateRect(h, NULL, FALSE);
}

/* ---- Controller page: the driving map (as the in-game BUTTON MAPPING) ---- */
static const struct { uint64_t bit; const char *name; } BUTTONS[] = {
    {0, "(none)"},
    {HidNpadButton_A, "A / Cross"}, {HidNpadButton_B, "B / Circle"}, {HidNpadButton_X, "X / Square"},
    {HidNpadButton_Y, "Y / Triangle"}, {HidNpadButton_L, "L / LB / L1"}, {HidNpadButton_R, "R / RB / R1"},
    {HidNpadButton_ZL, "ZL / LT / L2"}, {HidNpadButton_ZR, "ZR / RT / R2"}, {HidNpadButton_Plus, "+ / Start"},
    {HidNpadButton_StickL, "L stick press"}, {HidNpadButton_StickR, "R stick press"},
};
#define NBUTTONS (int)(sizeof BUTTONS / sizeof *BUTTONS)
static const char *const MAP_LABELS[MAP_COUNT] = {
    "A (accelerate)", "B (brake)", "X (item)", "Y (item, rear view)", "Z (switch drivers)", "L (drift)", "R (drift)", "Start (pause)",
    "Driver first person", "Jump", "Camera reset", "Kart first person",
};
/* a mask's first and second button (the page shows two per action; more stay as they are) */
static void map_show(void) {
    const uint64_t *s_map = map_cur();
    char t[48];
    snprintf(t, sizeof t, "Driving buttons: player %d", s_map_pl + 1);
    if (c_map_head) SetWindowTextA(c_map_head, t), InvalidateRect(c_map_head, NULL, TRUE);
    if (c_pads) InvalidateRect(c_pads, NULL, FALSE); /* its "in a race:" text */
    for (int r = 0; r < MAP_COUNT; r++) {
        int sel[2] = {0, 0}, n = 0;
        for (int i = 1; i < NBUTTONS && n < 2; i++)
            if (s_map[r] & BUTTONS[i].bit) sel[n++] = i;
        combo_set(c_map[r][0], sel[0]);
        combo_set(c_map[r][1], sel[1]);
    }
}
static void map_changed(int r) {
    uint64_t *s_map = map_edit(), known = 0, m = 0;
    for (int i = 1; i < NBUTTONS; i++) known |= BUTTONS[i].bit;
    for (int j = 0; j < 2; j++) m |= BUTTONS[combo_get(c_map[r][j])].bit;
    uint64_t old = s_map[r] & known, gained = m & ~old, lost = old & ~m;
    s_map[r] = (s_map[r] & ~known) | m;
    /* A button does one thing: giving it to this action takes it from whichever had it. That one
     * is left with the button this action gave up when it would otherwise have none, so
     * "A -> B" on one row and "B -> A" on the other is one swap, never two actions on one button
     * and an action with no button. */
    for (int q = 0; q < MAP_COUNT; q++) {
        if (q == r || !(s_map[q] & gained & known)) continue;
        s_map[q] &= ~gained;
        if (!(s_map[q] & known) && lost) s_map[q] |= lost, lost = 0;
    }
    map_show();
}

/* The live input view (as Dolphin's): sticks, d-pad, face buttons, shoulders and analog triggers
 * of the controller last used. s_view is what was drawn, to repaint only on a change. */
static int s_view_slot = -1;
static Pad s_view;
/* The controller list above the view ("Player N: name", by player) and the shown one's player box;
 * the list is rebuilt when a controller comes, goes or changes player. s_sel_slot[i]: list item i's slot. */
static int s_sel_slot[PAD_SLOTS], s_sel_n;
static void pad_sel_update(void) {
    static char sig[PAD_SLOTS * 72];
    char now[sizeof sig] = "";
    int n = 0, order[PAD_SLOTS];
    for (int pl = 0; pl < PAD_SLOTS; pl++)
        for (int s = 0; s < PAD_SLOTS; s++)
            if (g_pads[s].on && g_pads[s].player == pl) order[n++] = s;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(now);
        snprintf(now + l, sizeof now - l, "%d:%d:%.60s|", order[i], g_pads[order[i]].player, g_pads[order[i]].name);
    }
    if (strcmp(now, sig) || n != s_sel_n) {
        memcpy(sig, now, sizeof sig);
        SendMessageA(c_pad_sel, CB_RESETCONTENT, 0, 0);
        for (int i = 0; i < n; i++) {
            char t[96];
            snprintf(t, sizeof t, "Player %d: %s", g_pads[order[i]].player + 1, g_pads[order[i]].name);
            combo_add(c_pad_sel, t);
            s_sel_slot[i] = order[i];
        }
        if (!n) combo_add(c_pad_sel, "No controller (keyboard: player 1)");
        s_sel_n = n;
        EnableWindow(c_pad_sel, n > 1);
    }
    int at = 0;
    for (int i = 0; i < n; i++)
        if (s_sel_slot[i] == s_view_slot) at = i;
    if ((int)SendMessageA(c_pad_sel, CB_GETCURSEL, 0, 0) != at && SendMessageA(c_pad_sel, CB_GETDROPPEDSTATE, 0, 0) == 0) combo_set(c_pad_sel, at);
    int pl = s_view_slot >= 0 ? g_pads[s_view_slot].player : -1;
    if ((int)SendMessageA(c_pad_player, CB_GETCURSEL, 0, 0) != pl && SendMessageA(c_pad_player, CB_GETDROPPEDSTATE, 0, 0) == 0) combo_set(c_pad_player, pl);
    EnableWindow(c_pad_player, s_view_slot >= 0);
    /* the driving buttons below are this player's (player 1's with no controller: the keyboard) */
    int mp = pl >= 0 && pl < 4 ? pl : 0;
    if (mp != s_map_pl) s_map_pl = mp, map_show();
}
/* the shown controller becomes player pl; the one that was swaps to its old number */
static void pad_set_player(int pl) {
    if (s_view_slot < 0 || pl < 0 || pl >= PAD_SLOTS) return;
    int old = g_pads[s_view_slot].player;
    for (int s = 0; s < PAD_SLOTS; s++)
        if (s != s_view_slot && g_pads[s].on && g_pads[s].player == pl) g_pads[s].player = old;
    g_pads[s_view_slot].player = pl;
    rt_log("pad: launcher: %s is player %d", g_pads[s_view_slot].name, pl + 1);
    InvalidateRect(c_pads, NULL, FALSE);
}
static void pad_view_update(void) {
    int slot = s_view_slot >= 0 && s_view_slot < PAD_SLOTS && g_pads[s_view_slot].on ? s_view_slot : -1;
    for (int s = 0; s < PAD_SLOTS; s++) /* the one with a new press, else the first connected */
        if (g_pads[s].on && ((g_pads[s].b & ~g_pads[s].prev) || slot < 0)) slot = s;
    s_view_slot = slot;
    pad_sel_update();
    Pad now;
    memset(&now, 0, sizeof now);
    if (slot >= 0) now = g_pads[slot];
    now.prev = 0;
    if (memcmp(&now, &s_view, sizeof now)) s_view = now, InvalidateRect(c_pads, NULL, FALSE);
}
static void stick_draw(HDC dc, int cx, int cy, int r, int32_t x, int32_t y, bool press, const char *name) {
    circle(dc, cx - r, cy - r, 2 * r, press ? C_RED : C_BTN);
    circle(dc, cx - r + S(4), cy - r + S(4), 2 * r - S(8), press ? C_RED_DOWN : C_FIELD);
    int dx = (int)((int64_t)x * (r - S(10)) / 32767), dy = (int)((int64_t)-y * (r - S(10)) / 32767), d = S(18);
    circle(dc, cx + dx - d / 2, cy + dy - d / 2, d, x || y ? C_GOLD : C_TEXT);
    text(dc, name, (RECT){cx - S(50), cy + r + S(4), cx + S(50), cy + r + S(22)}, s_font, C_DIM, DT_SINGLELINE | DT_CENTER);
}
static void key_draw(HDC dc, RECT r, const char *t, bool on, int rad) {
    round_rect(dc, r, rad, on ? C_RED : C_BTN);
    text(dc, t, r, s_bold, on ? RGB(255, 255, 255) : C_DIM, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
}
static void trigger_draw(HDC dc, RECT r, const char *t, int v, bool on) {
    round_rect(dc, r, S(8), C_BTN);
    int w = (int)((r.right - r.left) * (long)v / 255);
    if (w > S(6)) round_rect(dc, (RECT){r.left, r.top, r.left + w, r.bottom}, S(8), on ? C_RED : RGB(150, 40, 52));
    text(dc, t, r, s_bold, on ? RGB(255, 255, 255) : C_DIM, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
}
static void pad_view_draw(const DRAWITEMSTRUCT *di) {
    RECT r = di->rcItem;
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC dc = CreateCompatibleDC(di->hDC);
    HBITMAP bm = CreateCompatibleBitmap(di->hDC, w, h);
    if (!dc || !bm) { if (bm) DeleteObject(bm); if (dc) DeleteDC(dc); return; }
    HGDIOBJ obm = SelectObject(dc, bm);
    fill(dc, (RECT){0, 0, w, h}, C_PANEL);
    round_rect(dc, (RECT){0, 0, w, h}, S(14), C_FIELD);
    const Pad *p = &s_view;
    char t[160];
    if (s_view_slot < 0) {
        text(dc, "No controller connected: the keyboard is player 1.", (RECT){0, 0, w, h - S(16)}, s_nav, C_DIM, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        text(dc, "Plug one in: it shows up here, with every button, stick and trigger live.", (RECT){0, S(24), w, h}, s_font, C_DIM,
             DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    } else {
        /* right: what the held buttons do in a race (this player's driving buttons), else the keyboard note */
        char act[96] = "";
        const uint64_t *m = map_cur();
        for (int r = 0; r < MAP_COUNT; r++)
            if (m[r] & p->b) {
                size_t l = strlen(act);
                snprintf(act + l, sizeof act - l, "%s%s", l ? " + " : "in a race: ", MAP_LABELS[r]);
            }
        const char *kb = act[0] ? act : p->player == 0 ? "the keyboard is player 1 too" : "";
        RECT kr = {S(14), S(6), w - S(14), S(28)};
        text(dc, kb, kr, act[0] ? s_bold : s_font, act[0] ? C_GOLD : C_DIM, DT_SINGLELINE | DT_RIGHT | DT_END_ELLIPSIS);
        snprintf(t, sizeof t, "Player %d:  %s%s", p->player + 1, p->name, p->gc ? "  (GameCube layout)" : "");
        text(dc, t, (RECT){S(14), S(6), w - S(14) - (act[0] ? S(330) : kb[0] ? S(200) : 0), S(28)}, s_nav, C_TEXT, DT_SINGLELINE | DT_END_ELLIPSIS);
        uint64_t b = p->b;
        int top = S(36), cy = top + S(46), sr = S(36);
        /* left stick, d-pad, right stick, face buttons */
        stick_draw(dc, S(60), cy, sr, p->lx, p->ly, b & HidNpadButton_StickL, "L stick");
        int dx = S(158), a = S(15), arm = S(28);
        key_draw(dc, (RECT){dx - a, cy - a - arm, dx + a, cy - a + S(2)}, "", b & HidNpadButton_Up, S(6));
        key_draw(dc, (RECT){dx - a, cy + a - S(2), dx + a, cy + a + arm}, "", b & HidNpadButton_Down, S(6));
        key_draw(dc, (RECT){dx - a - arm, cy - a, dx - a + S(2), cy + a}, "", b & HidNpadButton_Left, S(6));
        key_draw(dc, (RECT){dx + a - S(2), cy - a, dx + a + arm, cy + a}, "", b & HidNpadButton_Right, S(6));
        fill(dc, (RECT){dx - a + S(2), cy - a + S(2), dx + a - S(2), cy + a - S(2)}, C_BTN);
        text(dc, "D-pad", (RECT){dx - S(50), cy + sr + S(4), dx + S(50), cy + sr + S(22)}, s_font, C_DIM, DT_SINGLELINE | DT_CENTER);
        stick_draw(dc, S(256), cy, sr, p->rx, p->ry, b & HidNpadButton_StickR, "R stick / C");
        int fx = S(360), fd = S(28), fo = S(24);
        static const struct { uint64_t bit; const char *n; int x, y; } FACE_NIN[] = {
            {HidNpadButton_X, "X", 0, -1}, {HidNpadButton_A, "A", 1, 0}, {HidNpadButton_B, "B", 0, 1}, {HidNpadButton_Y, "Y", -1, 0},
        }, FACE_XBOX[] = { /* Xbox / PlayStation: the same names, in their places */
            {HidNpadButton_Y, "Y", 0, -1}, {HidNpadButton_B, "B", 1, 0}, {HidNpadButton_A, "A", 0, 1}, {HidNpadButton_X, "X", -1, 0},
        };
        const __typeof__(FACE_NIN[0]) *FACE = p->xface ? FACE_XBOX : FACE_NIN;
        for (int i = 0; i < 4; i++) {
            int x = fx + FACE[i].x * fo - fd / 2, y = cy + FACE[i].y * fo - fd / 2;
            bool on = b & FACE[i].bit;
            circle(dc, x, y, fd, on ? C_RED : C_BTN);
            text(dc, FACE[i].n, (RECT){x, y, x + fd, y + fd}, s_bold, on ? RGB(255, 255, 255) : C_DIM, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }
        text(dc, "Buttons", (RECT){fx - S(50), cy + sr + S(4), fx + S(50), cy + sr + S(22)}, s_font, C_DIM, DT_SINGLELINE | DT_CENTER);
        /* shoulders, analog triggers, + and - */
        int sx = S(430), sw = (w - S(14) - sx - S(8)) / 2, rh = S(24), ry = top + S(4);
        key_draw(dc, (RECT){sx, ry, sx + sw, ry + rh}, "L", b & HidNpadButton_L, S(8));
        key_draw(dc, (RECT){sx + sw + S(8), ry, sx + 2 * sw + S(8), ry + rh}, "R", b & HidNpadButton_R, S(8));
        ry += rh + S(8);
        trigger_draw(dc, (RECT){sx, ry, sx + sw, ry + rh}, "ZL", p->lt, b & HidNpadButton_ZL);
        trigger_draw(dc, (RECT){sx + sw + S(8), ry, sx + 2 * sw + S(8), ry + rh}, "ZR", p->rt, b & HidNpadButton_ZR);
        ry += rh + S(8);
        key_draw(dc, (RECT){sx, ry, sx + sw, ry + rh}, "-", b & HidNpadButton_Minus, S(8));
        key_draw(dc, (RECT){sx + sw + S(8), ry, sx + 2 * sw + S(8), ry + rh}, "+", b & HidNpadButton_Plus, S(8));
    }
    BitBlt(di->hDC, r.left, r.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obm);
    DeleteObject(bm), DeleteDC(dc);
}

/* Map rows light up while their buttons are held; a controller button pressed while a map box
 * has the focus becomes that box's button (click a box, press the button). */
static HWND c_map_lbl[MAP_COUNT];
static void map_live(void) {
    /* the shown controller only: the rows are its player's */
    uint64_t held = 0, pressed = 0;
    if (s_view_slot >= 0 && s_view_slot < PAD_SLOTS && g_pads[s_view_slot].on)
        held = g_pads[s_view_slot].b, pressed = held & ~g_pads[s_view_slot].prev;
    const uint64_t *s_map = map_cur();
    HWND f = GetFocus();
    for (int r = 0; r < MAP_COUNT && pressed; r++)
        for (int j = 0; j < 2; j++) {
            if (!f || c_map[r][j] != f) continue;
            for (int i = 1; i < NBUTTONS; i++)
                if (pressed & BUTTONS[i].bit) {
                    SendMessageA(f, CB_SHOWDROPDOWN, FALSE, 0);
                    combo_set(f, i);
                    map_changed(r);
                    SetFocus(s_wnd); /* done: the next press tests the button, it doesn't replace it */
                    break;
                }
        }
    for (int r = 0; r < MAP_COUNT; r++) {
        bool lit = (s_map[r] & held) != 0;
        LONG_PTR in = info(c_map_lbl[r]);
        if (!c_map_lbl[r] || lit == ((in & F_LIT) != 0)) continue;
        SetWindowLongPtrA(c_map_lbl[r], GWLP_USERDATA, in ^ F_LIT);
        InvalidateRect(c_map_lbl[r], NULL, TRUE);
    }
}

/* ---- Keyboard page ---- */
static void key_show(int r, int j) {
    char n[48];
    if (s_capture == r * KB_KEYS + j) snprintf(n, sizeof n, "press a key...");
    else wincfg_key_name(g_win.keys[r][j], n, sizeof n);
    SetWindowTextA(c_key[r][j], n[0] ? n : "-");
    InvalidateRect(c_key[r][j], NULL, FALSE);
}
static void keys_show(void) {
    for (int r = 0; r < KB_COUNT; r++)
        for (int j = 0; j < KB_KEYS; j++) key_show(r, j);
}

/* ---- Mods page ---- */
static unsigned s_found;
static int s_mod_sel[2 + 32]; /* combo index -> 0 auto, 1 off, 2 + n - 1 = MODS[n - 1] */
static int count_entries(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}
static void mods_fill(void) {
    s_found = find_mods();
    SendMessageA(c_mod, CB_RESETCONTENT, 0, 0);
    SendMessageA(c_mods, LB_RESETCONTENT, 0, 0);
    int k = 0, sel = 0;
    combo_add(c_mod, "Automatic (the first one installed)"), s_mod_sel[k++] = 0;
    combo_add(c_mod, "Off (the standard game)"), s_mod_sel[k++] = 1;
    if (!strcmp(g_set.mod, "off")) sel = 1;
    for (unsigned i = 0; i < MOD_COUNT && i < 32; i++) {
        bool in = s_found >> i & 1;
        char t[160];
        snprintf(t, sizeof t, "%s   -   %s", MODS[i].name, in ? "installed (mods\\" : "not installed");
        if (in) snprintf(t + strlen(t), sizeof t - strlen(t), "%s)", MODS[i].key);
        SendMessageA(c_mods, LB_ADDSTRING, 0, (LPARAM)t);
        if (!in && strcmp(g_set.mod, MODS[i].key)) continue;
        snprintf(t, sizeof t, "%s%s", MODS[i].name, in ? "" : " (not installed)");
        combo_add(c_mod, t);
        if (!strcmp(g_set.mod, MODS[i].key)) sel = k;
        s_mod_sel[k++] = 2 + (int)i;
    }
    if (!MOD_COUNT) SendMessageA(c_mods, LB_ADDSTRING, 0, (LPARAM) "(this build has no mod support)");
    combo_set(c_mod, sel);
    /* languages: a PAL disc's, extracted by MKDD-NX Tools into mods\lang */
    unsigned langs = langs_find(MODS_DIR);
    SendMessageA(c_lang, CB_RESETCONTENT, 0, 0);
    k = sel = 0;
    for (int i = 0; i < LANG_COUNT; i++) {
        bool in = !i || (langs >> i & 1);
        if (!in && g_set.language != i) continue;
        char t[64];
        snprintf(t, sizeof t, "%s%s", LANG_NAMES[i], in ? "" : " (not installed)");
        combo_add(c_lang, t);
        if (g_set.language == i) sel = k;
        s_lang_sel[k++] = i;
    }
    combo_set(c_lang, sel);
}
static bool install_progress(void *ud, uint64_t done, uint64_t total) {
    (void)ud;
    SendMessageA(c_progress, PBM_SETPOS, total ? (WPARAM)(done * 1000 / total) : 0, 0);
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) return false;
        TranslateMessage(&m), DispatchMessageA(&m);
    }
    return !s_done;
}
static const char *disc_choice(void);
static void install_mod(void) {
    char path[MAX_PATH] = "", err[256];
    OPENFILENAMEA of = {sizeof of};
    of.hwndOwner = s_wnd;
    of.lpstrFilter = "Mod patches and patched discs (*.xdelta;*.iso;*.gcm;*.ciso;*.rvz)\0*.xdelta;*.iso;*.gcm;*.ciso;*.rvz\0All files\0*.*\0";
    of.lpstrFile = path, of.nMaxFile = sizeof path;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&of)) return;
    char disc[MAX_PATH];
    const char *d = disc_choice();
    if (d) snprintf(disc, sizeof disc, "%s", d);
    else if (!find_disc(disc, sizeof disc)) {
        MessageBoxA(s_wnd, "Installing a mod needs your Mario Kart: Double Dash!! (USA) disc image: put it next to MKDD-NX.exe or pick it on the Game page.", APP_TITLE, MB_ICONWARNING);
        return;
    }
    if (!disc_open(disc, err, sizeof err)) { MessageBoxA(s_wnd, err, APP_TITLE, MB_ICONERROR); return; }
    if (!s_disc_locked) { /* the game plays the disc image opened here */
        s_disc_locked = true;
        EnableWindow(c_disc, FALSE);
    }
    SetWindowTextA(c_status, "Checking the file...");
    unsigned n = mod_identify(path, err, sizeof err);
    if (!n) { SetWindowTextA(c_status, ""); MessageBoxA(s_wnd, err, APP_TITLE, MB_ICONWARNING); return; }
    const ModInfo *mi = &MODS[n - 1];
    char q[300], out[300];
    snprintf(q, sizeof q, "Install %s?\nIt takes a few minutes and a few GB in the mods folder.", mi->name);
    if (MessageBoxA(s_wnd, q, APP_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) { SetWindowTextA(c_status, ""); return; }
    snprintf(out, sizeof out, MODS_DIR "/%s", mi->key);
    snprintf(q, sizeof q, "Installing %s...", mi->name);
    SetWindowTextA(c_status, q);
    s_installing = true;
    ShowWindow(c_progress, SW_SHOW);
    EnableWindow(GetDlgItem(s_wnd, ID_INSTALL), FALSE), EnableWindow(GetDlgItem(s_wnd, ID_PLAY), FALSE);
    unsigned ok = mod_install(path, out, install_progress, NULL, err, sizeof err);
    EnableWindow(GetDlgItem(s_wnd, ID_INSTALL), TRUE), EnableWindow(GetDlgItem(s_wnd, ID_PLAY), TRUE);
    s_installing = false;
    ShowWindow(c_progress, SW_HIDE);
    if (s_done) return;
    if (!ok) { SetWindowTextA(c_status, ""); MessageBoxA(s_wnd, err, "The mod was not installed", MB_ICONERROR); return; }
    snprintf(g_set.mod, sizeof g_set.mod, "%s", mi->key); /* play what was just installed */
    snprintf(q, sizeof q, "%s is installed.", mi->name);
    SetWindowTextA(c_status, q);
    mods_fill();
}

/* ---- Game page: disc images ---- */
static void discs_fill(void) {
    static const char *EXT[] = {".iso", ".gcm", ".ciso", ".rvz"};
    s_ndiscs = 0;
    snprintf(s_discs[s_ndiscs++], sizeof *s_discs, "%s", "");
    combo_add(c_disc, "Automatic (the disc image next to MKDD-NX.exe)");
    int sel = 0;
    DIR *d = opendir(".");
    struct dirent *e;
    while (d && (e = readdir(d)) && s_ndiscs < 15) {
        const char *dot = strrchr(e->d_name, '.');
        for (int i = 0; dot && i < 4; i++)
            if (!strcasecmp(dot, EXT[i])) {
                char full[MAX_PATH];
                if (!GetFullPathNameA(e->d_name, sizeof full, full, NULL)) break;
                snprintf(s_discs[s_ndiscs], sizeof *s_discs, "%s", full);
                combo_add(c_disc, e->d_name);
                if (!strcasecmp(full, g_win.disc)) sel = s_ndiscs;
                s_ndiscs++;
                break;
            }
    }
    if (d) closedir(d);
    if (g_win.disc[0] && !sel && access(g_win.disc, R_OK) == 0 && s_ndiscs < 16) {
        snprintf(s_discs[s_ndiscs], sizeof *s_discs, "%s", g_win.disc);
        combo_add(c_disc, g_win.disc);
        sel = s_ndiscs++;
    }
    combo_set(c_disc, sel);
}
static const char *disc_choice(void) {
    int i = combo_get(c_disc);
    return i > 0 && i < s_ndiscs ? s_discs[i] : NULL;
}
static void browse_disc(void) {
    char path[MAX_PATH] = "";
    OPENFILENAMEA of = {sizeof of};
    of.hwndOwner = s_wnd;
    of.lpstrFilter = "GameCube disc images (*.iso;*.gcm;*.ciso;*.rvz)\0*.iso;*.gcm;*.ciso;*.rvz\0All files\0*.*\0";
    of.lpstrFile = path, of.nMaxFile = sizeof path;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&of)) return;
    for (int i = 1; i < s_ndiscs; i++)
        if (!strcasecmp(s_discs[i], path)) { combo_set(c_disc, i); return; }
    if (s_ndiscs >= 16) s_ndiscs = 15, SendMessageA(c_disc, CB_DELETESTRING, 15, 0);
    snprintf(s_discs[s_ndiscs], sizeof *s_discs, "%s", path);
    combo_add(c_disc, path);
    combo_set(c_disc, s_ndiscs++);
}

/* ---- the window ---- */
static const int WIN_SIZES[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
#define NSIZES (int)(sizeof WIN_SIZES / sizeof *WIN_SIZES)
static const int RES_VALUES[] = {DOCKED_AUTO, DOCKED_NATIVE, DOCKED_1_5X, DOCKED_2X, DOCKED_3X, DOCKED_4X};
static char s_devs[32][MAXPNAMELEN];
static int s_ndevs;

static void build(void) {
    const int L = PANEL_X + 24, T = PANEL_Y + 20, W = 640; /* page area: left, top, width */
    for (int i = 0; i < P_COUNT; i++)
        c_nav[i] = kind(add(-1, "BUTTON", PAGE_NAMES[i], WS_TABSTOP | BS_OWNERDRAW, SIDE_X, PANEL_Y + i * 46, SIDE_W, 42, ID_NAV0 + i), K_NAV);

    /* Game */
    int y = T;
    heading(P_GAME, "Disc image", L, y), y += 26;
    c_disc = combo(P_GAME, L, y, 500, ID_DISC);
    button(P_GAME, "Browse...", L + 510, y - 1, 110, ID_BROWSE), y += 32;
    note(P_GAME, "Your own Mario Kart: Double Dash!! (USA, GM4E01) as .iso, .gcm, .ciso or .rvz.", L, y, W - 20, 20), y += 36;
    discs_fill();
    heading(P_GAME, "Game", L, y), y += 26;
    c_wide = toggle(P_GAME, "Widescreen (16:9)", g_set.wide, L, y, 280);
    label(P_GAME, "Frame rate counter", L + 300, y, 140);
    static const char *const FPS[] = {"Off", "FPS", "FPS and frame times"};
    c_fps = combo_list(P_GAME, L + 440, y, 180, FPS, 3, g_set.show_fps), y += 40;
    heading(P_GAME, "LAN play", L, y), y += 26;
    c_lan = toggle(P_GAME, "LAN play (the game's Broadband Adapter LAN mode)", g_set.lan, L, y, 460), y += 32;
    label(P_GAME, "Other computer or Switch's address (only for networks that block broadcasts):", L, y, W - 20), y += 24;
    c_peer = add(P_GAME, "EDIT", g_set.lan_peer, WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, L, y, 220, 24, 0);
    SendMessageA(c_peer, EM_LIMITTEXT, sizeof g_set.lan_peer - 1, 0);
    note(P_GAME, "a.b.c.d or a.b.c.d:port; empty: none", L + 234, y + 3, 300, 20), y += 42;
    heading(P_GAME, "Start-up", L, y), y += 26;
    c_show = toggle(P_GAME, "Show this launcher when the game starts (hold Shift to show it anyway)", g_win.launcher, L, y, W - 20), y += 34;
    label(P_GAME, "Log file (logs\\mkdd.log)", L, y, 170);
    static const char *const LOGS[] = {"Off", "Game", "Game and audio", "Audio", "Game and battle AI"};
    c_log = combo_list(P_GAME, L + 180, y, 200, LOGS, 5, g_set.logging), y += 40;
    button(P_GAME, "Open the game folder", L, y, 180, ID_OPEN_DIR);

    /* Graphics */
    y = T;
    heading(P_GFX, "Window", L, y), y += 26;
    label(P_GFX, "Window size", L, y, 170);
    c_winsize = combo(P_GFX, L + 180, y, 160, 0);
    int wsel = -1;
    for (int i = 0; i < NSIZES; i++) {
        char t[32];
        snprintf(t, sizeof t, "%d x %d", WIN_SIZES[i][0], WIN_SIZES[i][1]);
        combo_add(c_winsize, t);
        if (WIN_SIZES[i][0] == g_win.win_w && WIN_SIZES[i][1] == g_win.win_h) wsel = i;
    }
    if (wsel < 0) {
        char t[32];
        snprintf(t, sizeof t, "%d x %d", g_win.win_w, g_win.win_h);
        combo_add(c_winsize, t), wsel = NSIZES;
    }
    combo_set(c_winsize, wsel);
    c_full = toggle(P_GFX, "Full screen (F11 / Alt+Enter)", g_win.fullscreen, L + 360, y, 260), y += 42;
    heading(P_GFX, "Picture", L, y), y += 26;
    label(P_GFX, "Render resolution", L, y, 170);
    static const char *const RES[] = {"Automatic (fits the window)", "Native (640 x 480)", "1.5x (960 x 720)", "2x (1280 x 960)", "3x (1920 x 1440)", "4x (2560 x 1920)"};
    int rsel = 0;
    for (int i = 0; i < 6; i++)
        if (RES_VALUES[i] == g_set.docked_res) rsel = i;
    c_res = combo_list(P_GFX, L + 180, y, 240, RES, 6, rsel), y += 34;
    label(P_GFX, "Texture filtering", L, y, 170);
    static const char *const ANISO[] = {"Normal", "Anisotropic 2x", "Anisotropic 4x", "Anisotropic 8x", "Anisotropic 16x"};
    c_aniso = combo_list(P_GFX, L + 180, y, 240, ANISO, 5, g_set.aniso), y += 34;
    static const char *const FOV[] = {"Normal (the game's)", "Wide", "Wider", "Widest", "Widest+"};
    label(P_GFX, "Field of view, 16:9", L, y, 170);
    c_fov169 = combo_list(P_GFX, L + 180, y, 240, FOV, 5, g_set.fov[1]), y += 34;
    label(P_GFX, "Field of view, 4:3", L, y, 170);
    c_fov43 = combo_list(P_GFX, L + 180, y, 240, FOV, 5, g_set.fov[0]), y += 40;
    c_border = toggle(P_GFX, "4:3: checkered border art at the sides in races", g_set.border, L, y, W - 20), y += 40;
    note(P_GFX, "HD textures are on the Mods page. More picture settings (HUD, camera, colors) are in the game's menu: press - (Back, Esc).", L, y,
         W - 20, 40);

    /* Audio */
    y = T;
    heading(P_AUDIO, "Sound", L, y), y += 26;
    label(P_AUDIO, "Output device", L, y, 170);
    c_dev = combo(P_AUDIO, L + 180, y, 400, 0);
    combo_add(c_dev, "The system's default");
    int dsel = 0;
    s_ndevs = 0;
    for (UINT i = 0, n = waveOutGetNumDevs(); i < n && s_ndevs < 32; i++) {
        WAVEOUTCAPSA caps;
        if (waveOutGetDevCapsA(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
        snprintf(s_devs[s_ndevs], sizeof *s_devs, "%s", caps.szPname);
        combo_add(c_dev, caps.szPname);
        if (!strcmp(caps.szPname, g_win.audio_dev)) dsel = s_ndevs + 1;
        s_ndevs++;
    }
    combo_set(c_dev, dsel), y += 38;
    label(P_AUDIO, "Volume", L, y, 170);
    c_vol = add(P_AUDIO, TRACKBAR_CLASSA, "", WS_TABSTOP | TBS_NOTICKS, L + 174, y - 2, 320, 30, ID_VOLUME);
    SendMessageA(c_vol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageA(c_vol, TBM_SETPOS, TRUE, g_win.volume);
    c_vol_txt = label(P_AUDIO, "", L + 506, y, 60), y += 38;
    char vt[16];
    snprintf(vt, sizeof vt, "%d%%", g_win.volume);
    SetWindowTextA(c_vol_txt, vt);
    label(P_AUDIO, "Latency", L, y, 170);
    static const char *const LAT[] = {"Low (43 ms; may crackle)", "Normal (85 ms)", "High (128 ms; for slow PCs)"};
    c_lat = combo_list(P_AUDIO, L + 180, y, 280, LAT, 3, g_win.latency), y += 48;
    heading(P_AUDIO, "Music", L, y), y += 26;
    const char *audio = find_audio();
    c_music = toggle(P_AUDIO, audio ? "Stereo music for MKDD Plus (mods\\audio)" : "Stereo music for MKDD Plus (none yet: mods\\audio)", g_set.audio, L, y, W - 20);

    /* Controller */
    y = T;
    HWND hd = heading(P_CTRL, "Live input", L, y);
    if (hd) SetWindowPos(hd, NULL, 0, 0, S(96), S(22), SWP_NOMOVE | SWP_NOZORDER);
    /* which controller is shown (the last one pressed, or picked here) and which player it is */
    c_pad_sel = combo(P_CTRL, L + 100, y - 4, 330, ID_PAD_VIEW);
    label(P_CTRL, "is player", L + 442, y - 2, 64);
    c_pad_player = combo(P_CTRL, L + 508, y - 4, 112, ID_PAD_PLAYER);
    for (int i = 0; i < PAD_SLOTS; i++) {
        char t[16];
        snprintf(t, sizeof t, "Player %d", i + 1);
        combo_add(c_pad_player, t);
    }
    y += 28;
    c_pads = kind(add(P_CTRL, "STATIC", "", SS_OWNERDRAW, L, y, W - 20, 150, 0), K_PADVIEW), y += 162;
    c_map_head = heading(P_CTRL, "Driving buttons: player 1", L, y);
    if (c_map_head) SetWindowPos(c_map_head, NULL, 0, 0, S(220), S(22), SWP_NOMOVE | SWP_NOZORDER);
    note(P_CTRL, "click a box, then press a controller button (or pick from the list)", L + 226, y + 2, W - 246, 20), y += 28;
    memcpy(s_maps[0], g_set.map, sizeof s_maps[0]);
    s_map_own[0] = true;
    for (int pl = 1; pl < 4; pl++)
        if ((s_map_own[pl] = g_set.pl[pl - 1].own)) memcpy(s_maps[pl], g_set.pl[pl - 1].map, sizeof s_maps[pl]);
    for (int r = 0; r < MAP_COUNT; r++) {
        int col = r / 6, x = L + col * 322, yy = y + (r % 6) * 30;
        c_map_lbl[r] = label(P_CTRL, MAP_LABELS[r], x, yy, 116);
        for (int j = 0; j < 2; j++) {
            c_map[r][j] = combo(P_CTRL, x + 116 + j * 101, yy, 99, ID_MAP0 + r * 2 + j);
            SendMessageA(c_map[r][j], CB_SETDROPPEDWIDTH, (WPARAM)S(180), 0);
            for (int i = 0; i < NBUTTONS; i++) combo_add(c_map[r][j], BUTTONS[i].name);
        }
    }
    map_show();
    y += 6 * 30 + 4;
    button(P_CTRL, "Default buttons", L, y, 140, ID_MAP_DEFAULT);
    label(P_CTRL, "Rumble", L + 156, y + 4, 56);
    static const char *const RUMBLE[] = {"Off", "Low", "High"};
    c_rumble = combo_list(P_CTRL, L + 214, y + 1, 90, RUMBLE, 3, g_set.rumble);
    note(P_CTRL, "Xbox, PlayStation, Switch, GameCube adapter. Others: config\\gamecontrollerdb.txt",
         L + 318, y - 2, W - 318, 40);

    /* Keyboard */
    y = T;
    heading(P_KEYS, "Keys for the controller buttons (player 1)", L, y), y += 24;
    note(P_KEYS, "Click a key, then press the new one. Backspace clears it.", L, y, W, 20), y += 28;
    for (int r = 0; r < KB_COUNT; r++) {
        int col = r / 12, x = L + col * 320, yy = y + (r % 12) * 27;
        label(P_KEYS, KB_NAMES[r], x, yy, 124);
        for (int j = 0; j < KB_KEYS; j++)
            c_key[r][j] = kind(add(P_KEYS, "BUTTON", "", WS_TABSTOP | BS_OWNERDRAW, x + 126 + j * 88, yy, 84, 24, ID_KEY0 + r * KB_KEYS + j), K_KEY);
    }
    keys_show();
    button(P_KEYS, "Default keys", L, y + 12 * 27 + 6, 140, ID_KEYS_DEFAULT);
    note(P_KEYS, "The game's own menu is on - (Esc by default). F11 / Alt+Enter: full screen.", L + 156, y + 12 * 27 + 11, W - 160, 20);

    /* Mods */
    y = T;
    heading(P_MODS, "Mod to play", L, y), y += 26;
    heading(P_MODS, "Language (standard game)", L + 400, y - 26);
    c_lang = combo(P_MODS, L + 400, y, W - 20 - 400, 0);
    c_mod = combo(P_MODS, L, y, 380, 0), y += 36;
    c_mods = add(P_MODS, "LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOSEL | LBS_NOINTEGRALHEIGHT, L, y, W - 20, 92, 0), y += 100;
    if (c_mods) SetWindowTheme(c_mods, L"DarkMode_Explorer", NULL);
    button(P_MODS, "Install a mod...", L, y, 150, ID_INSTALL);
    button(P_MODS, "Open the mods folder", L + 162, y, 180, ID_OPEN_MODS), y += 36;
    c_progress = add(P_MODS, PROGRESS_CLASSA, "", 0, L, y, W - 20, 10, 0);
    if (c_progress) {
        SetWindowTheme(c_progress, L"", L"");
        SendMessageA(c_progress, PBM_SETRANGE32, 0, 1000);
        SendMessageA(c_progress, PBM_SETBARCOLOR, 0, C_RED);
        SendMessageA(c_progress, PBM_SETBKCOLOR, 0, C_FIELD);
    }
    c_status = add(P_MODS, "STATIC", "", SS_NOPREFIX, L, y + 14, W - 20, 20, 0), y += 40;
    note(P_MODS, "Pick a mod's .xdelta patch, or a disc image with the mod already patched in (MKDD Plus, Retro Track GP...). "
                 "It is unpacked into mods\\; play the mod from there with your own clean disc image. Languages: "
                 "MKDD-NX Tools copies them from a PAL disc into mods\\lang.",
         L, y - 8, W - 20, 62), y += 64;
    heading(P_MODS, "Extras", L, y), y += 26;
    int nc = count_entries(MODS_DIR "/characters"), nk = count_entries(MODS_DIR "/karts");
    char t[128];
    snprintf(t, sizeof t, "Extra characters (mods\\characters: %d)", nc < 0 ? 0 : nc);
    c_chars = toggle(P_MODS, t, g_set.characters, L, y, W - 20), y += 30;
    snprintf(t, sizeof t, "Extra karts (mods\\karts: %d)", nk < 0 ? 0 : nk);
    c_karts = toggle(P_MODS, t, g_set.karts, L, y, W - 20), y += 30;
    struct _stat64 st;
    if (_stat64(MODS_DIR "/hd/" HD_FILE, &st) == 0)
        snprintf(t, sizeof t, "HD textures (mods\\hd\\" HD_FILE ": %lld MB)", (long long)(st.st_size >> 20));
    else
        snprintf(t, sizeof t, "HD textures (none yet: put the pack at mods\\hd\\" HD_FILE ")");
    c_hd = toggle(P_MODS, t, g_set.hd_textures, L, y, W - 20);
    mods_fill();

    HWND play = kind(add(-1, "BUTTON", "PLAY", WS_TABSTOP | BS_OWNERDRAW, SIDE_X, CH - 12 - 36 - 10 - 54, SIDE_W, 54, ID_PLAY), K_PLAY);
    kind(add(-1, "BUTTON", "Quit", WS_TABSTOP | BS_OWNERDRAW, SIDE_X, CH - 12 - 36, SIDE_W, 36, ID_QUIT), K_BTN);
    show_page(P_GAME);
    SetFocus(play);
}

/* PLAY: everything back into g_set / g_win, both files saved */
static void apply(void) {
    g_set.wide = checked(c_wide);
    g_set.show_fps = combo_get(c_fps);
    g_set.lan = checked(c_lan);
    GetWindowTextA(c_peer, g_set.lan_peer, sizeof g_set.lan_peer);
    g_set.logging = combo_get(c_log);
    g_win.launcher = checked(c_show);
    const char *d = disc_choice();
    snprintf(g_win.disc, sizeof g_win.disc, "%s", d ? d : "");
    int ws = combo_get(c_winsize);
    if (ws < NSIZES) g_win.win_w = WIN_SIZES[ws][0], g_win.win_h = WIN_SIZES[ws][1];
    g_win.fullscreen = checked(c_full);
    g_set.docked_res = RES_VALUES[combo_get(c_res)];
    g_set.aniso = combo_get(c_aniso);
    g_set.fov[1] = combo_get(c_fov169), g_set.fov[0] = combo_get(c_fov43);
    g_set.border = checked(c_border);
    g_set.hd_textures = checked(c_hd);
    int dv = combo_get(c_dev);
    snprintf(g_win.audio_dev, sizeof g_win.audio_dev, "%s", dv > 0 && dv <= s_ndevs ? s_devs[dv - 1] : "");
    g_win.volume = (int)SendMessageA(c_vol, TBM_GETPOS, 0, 0);
    g_win.latency = combo_get(c_lat);
    g_set.audio = checked(c_music);
    g_set.rumble = combo_get(c_rumble);
    memcpy(g_set.map, s_maps[0], sizeof g_set.map);
    for (int pl = 1; pl < 4; pl++) {
        PlayerSet *ps = &g_set.pl[pl - 1];
        if (!s_map_own[pl]) continue;
        if (!ps->own) player_set(0, ps), ps->own = true; /* their camera starts as player 1's */
        memcpy(ps->map, s_maps[pl], sizeof ps->map);
    }
    int m = s_mod_sel[combo_get(c_mod)];
    snprintf(g_set.mod, sizeof g_set.mod, "%s", m == 0 ? "auto" : m == 1 ? "off" : MODS[m - 2].key);
    int lg = combo_get(c_lang);
    g_set.language = lg >= 0 && lg < LANG_COUNT ? s_lang_sel[lg] : 0;
    g_set.characters = checked(c_chars);
    g_set.karts = checked(c_karts);
    settings_save();
    wincfg_save();
}

static void open_folder(const char *dir) {
    char full[MAX_PATH];
    if (GetFullPathNameA(dir, sizeof full, full, NULL)) ShellExecuteA(s_wnd, "open", full, NULL, NULL, SW_SHOWNORMAL);
}

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CLOSE: s_done = true; return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(w); return 0;
    case WM_DRAWITEM:
        if ((info(((const DRAWITEMSTRUCT *)lp)->hwndItem) & K_KIND) == K_PADVIEW) pad_view_draw((const DRAWITEMSTRUCT *)lp);
        else draw_item((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_CTLCOLORSTATIC: { /* labels on the panel: headings gold, notes dimmed, lit map rows gold */
        LONG_PTR in = info((HWND)lp);
        int k = (int)(in & K_KIND);
        SetTextColor((HDC)wp, k == K_HEAD || (in & F_LIT) ? C_GOLD : k == K_NOTE ? C_DIM : C_TEXT);
        SetBkColor((HDC)wp, C_PANEL);
        return (LRESULT)s_br_panel;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, C_TEXT);
        SetBkColor((HDC)wp, C_FIELD);
        return (LRESULT)s_br_field;
    case WM_SETCURSOR: set_hot((HWND)wp); break;
    case WM_NOTIFY: {
        NMHDR *h = (NMHDR *)lp;
        if (h->hwndFrom == c_vol && h->code == NM_CUSTOMDRAW) return slider_draw((NMCUSTOMDRAW *)lp);
        return 0;
    }
    case WM_HSCROLL:
        if ((HWND)lp == c_vol) {
            char t[16];
            snprintf(t, sizeof t, "%d%%", (int)SendMessageA(c_vol, TBM_GETPOS, 0, 0));
            SetWindowTextA(c_vol_txt, t);
            /* the trackbar repaints only the old and new knob spots; the red bar runs from the
             * left edge to the knob, so the whole control must repaint */
            InvalidateRect(c_vol, NULL, TRUE);
        }
        return 0;
    case WM_TIMER: {
        pad_poll();
        if (s_page == P_CTRL) pad_view_update(), map_live();
        POINT p;
        if (s_hot && GetCursorPos(&p) && WindowFromPoint(p) != s_hot) set_hot(NULL);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        bool click = code == BN_CLICKED || code == BN_DOUBLECLICKED;
        if (id == ID_PLAY || id == IDOK) { apply(); s_play = s_done = true; }
        else if (id == ID_QUIT) s_done = true;
        else if (id >= ID_NAV0 && id < ID_NAV0 + P_COUNT) { if (click) show_page(id - ID_NAV0); }
        else if (id >= ID_TOG0 && id < ID_TOG0 + s_ntog) {
            if (click && lp) SetWindowLongPtrA((HWND)lp, GWLP_USERDATA, info((HWND)lp) ^ F_ON), InvalidateRect((HWND)lp, NULL, FALSE);
        } else if (id == ID_BROWSE) browse_disc();
        else if (id == ID_OPEN_DIR) open_folder(".");
        else if (id == ID_OPEN_MODS) open_folder(MODS_DIR);
        else if (id == ID_INSTALL) install_mod();
        else if (id == ID_MAP_DEFAULT) memcpy(map_edit(), MAP_DRIVE_DEFAULT, sizeof s_maps[0]), map_show(); /* the shown player's only */
        else if (id == ID_KEYS_DEFAULT) wincfg_default_keys(), s_capture = -1, keys_show();
        else if (id == ID_PAD_VIEW && code == CBN_SELCHANGE) {
            int i = combo_get(c_pad_sel);
            if (i < s_sel_n) s_view_slot = s_sel_slot[i], pad_view_update();
        }
        else if (id == ID_PAD_PLAYER && code == CBN_SELCHANGE) pad_set_player(combo_get(c_pad_player)), pad_view_update();
        else if (id >= ID_MAP0 && id < ID_MAP0 + MAP_COUNT * 2 && code == CBN_SELCHANGE) map_changed((id - ID_MAP0) / 2);
        else if (id >= ID_MAP0 && id < ID_MAP0 + MAP_COUNT * 2 && code == CBN_CLOSEUP) SetFocus(s_wnd);
        else if (id >= ID_KEY0 && id < ID_KEY0 + KB_COUNT * KB_KEYS) {
            int old = s_capture;
            s_capture = id - ID_KEY0;
            if (old >= 0) key_show(old / KB_KEYS, old % KB_KEYS);
            key_show(s_capture / KB_KEYS, s_capture % KB_KEYS);
        }
        return 0;
    }
    }
    return DefWindowProcA(w, m, wp, lp);
}

/* a key for the button waiting for one (taken before the dialog sees it: Enter, Esc, Tab, arrows) */
static bool capture_key(const MSG *msg) {
    if (s_capture < 0 || (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN)) return false;
    int vk = (int)msg->wParam, r = s_capture / KB_KEYS, j = s_capture % KB_KEYS;
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) vk = VK_SHIFT;
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) vk = VK_CONTROL;
    if (vk == VK_LMENU || vk == VK_RMENU) vk = VK_MENU;
    if (vk == VK_F11) return true; /* the game's full-screen key */
    g_win.keys[r][j] = vk == VK_BACK ? 0 : vk;
    s_capture = -1;
    key_show(r, j);
    return true;
}

static HFONT font(int px, int weight, bool italic, const char *face) {
    return CreateFontA(-S(px), 0, 0, 0, weight, italic, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

bool launcher_run(void) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HDC dc = GetDC(NULL);
    s_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    if (s_dpi < 96) s_dpi = 96;
    s_font = font(14, FW_NORMAL, false, "Segoe UI");
    s_bold = font(15, FW_BOLD, false, "Segoe UI");
    s_nav = font(16, FW_SEMIBOLD, false, "Segoe UI");
    s_big = font(21, FW_BOLD, false, "Segoe UI");
    s_title = font(34, FW_BLACK, true, "Segoe UI Black");
    s_icons = font(17, FW_NORMAL, false, "Segoe MDL2 Assets");
    s_br_bg = CreateSolidBrush(C_BG), s_br_panel = CreateSolidBrush(C_PANEL), s_br_field = CreateSolidBrush(C_FIELD);

    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    wc.hbrBackground = s_br_bg;
    wc.lpszClassName = "MKDDNXLauncher";
    RegisterClassA(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = {0, 0, S(CW), S(CH)};
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    s_wnd = CreateWindowExA(WS_EX_CONTROLPARENT, "MKDDNXLauncher", APP_TITLE " Launcher", style, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2,
                            (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, NULL, NULL, inst, NULL);
    if (!s_wnd) { rt_log("launcher: CreateWindow failed (%lu)", GetLastError()); return true; }
    BOOL dark = TRUE; /* dark title bar: Windows 10 20H1+ (20), 1809-1909 (19) */
    if (FAILED(DwmSetWindowAttribute(s_wnd, 20, &dark, sizeof dark))) DwmSetWindowAttribute(s_wnd, 19, &dark, sizeof dark);
    build();
    SetTimer(s_wnd, 1, 25, NULL);
    ShowWindow(s_wnd, SW_SHOW);
    SetForegroundWindow(s_wnd);
    rt_log("launcher: open");
    MSG msg;
    while (!s_done && GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (capture_key(&msg)) continue;
        if (IsDialogMessageA(s_wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    KillTimer(s_wnd, 1);
    DestroyWindow(s_wnd);
    s_wnd = NULL;
    s_hot = NULL;
    HFONT fonts[] = {s_font, s_bold, s_nav, s_big, s_title, s_icons};
    for (size_t i = 0; i < sizeof fonts / sizeof *fonts; i++) DeleteObject(fonts[i]);
    DeleteObject(s_br_bg), DeleteObject(s_br_panel), DeleteObject(s_br_field);
    /* drop what the window left in the queue (a WM_QUIT would end the game's first frames) */
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {}
    rt_log("launcher: %s", s_play ? "play" : "quit");
    return s_play;
}
