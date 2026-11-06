/* The few libnx names the shared front-end files (menu.c, pause_art.c, charpack_art.c) use, for the
 * Windows build: integer types, Result, the HID button bits (main_win.c maps XInput and the keyboard
 * onto them, so the menu's BUTTON MAPPING keeps its meaning), the system tick and a nifm stand-in. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef u32 Result;
#define R_SUCCEEDED(rc) ((rc) == 0)
#define R_FAILED(rc) ((rc) != 0)
#define BITL(n) (1ull << (n))

/* libnx hid.h values */
enum {
    HidNpadButton_A = BITL(0), HidNpadButton_B = BITL(1), HidNpadButton_X = BITL(2), HidNpadButton_Y = BITL(3),
    HidNpadButton_StickL = BITL(4), HidNpadButton_StickR = BITL(5), HidNpadButton_L = BITL(6), HidNpadButton_R = BITL(7),
    HidNpadButton_ZL = BITL(8), HidNpadButton_ZR = BITL(9), HidNpadButton_Plus = BITL(10), HidNpadButton_Minus = BITL(11),
    HidNpadButton_Left = BITL(12), HidNpadButton_Up = BITL(13), HidNpadButton_Right = BITL(14), HidNpadButton_Down = BITL(15),
    HidNpadButton_StickLLeft = BITL(16), HidNpadButton_StickLUp = BITL(17), HidNpadButton_StickLRight = BITL(18),
    HidNpadButton_StickLDown = BITL(19), HidNpadButton_StickRLeft = BITL(20), HidNpadButton_StickRUp = BITL(21),
    HidNpadButton_StickRRight = BITL(22), HidNpadButton_StickRDown = BITL(23), HidNpadButton_LeftSL = BITL(24),
    HidNpadButton_LeftSR = BITL(25), HidNpadButton_RightSL = BITL(26), HidNpadButton_RightSR = BITL(27),
};
#define HidNpadButton_AnyLeft (HidNpadButton_Left | HidNpadButton_StickLLeft | HidNpadButton_StickRLeft)
#define HidNpadButton_AnyUp (HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp)
#define HidNpadButton_AnyRight (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_StickRRight)
#define HidNpadButton_AnyDown (HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown)
#define HidNpadButton_AnySL (HidNpadButton_LeftSL | HidNpadButton_RightSL)
#define HidNpadButton_AnySR (HidNpadButton_LeftSR | HidNpadButton_RightSR)

/* The Switch's 19.2 MHz system tick, from the performance counter (main_win.c) */
u64 armGetSystemTick(void);
static inline u64 armTicksToNs(u64 t) { return t * 625 / 12; }
static inline u64 armNsToTicks(u64 ns) { return ns * 12 / 625; }

/* nifm: this PC's IPv4 address (network byte order, as libnx gives it), for the menu's LAN page */
enum { NifmServiceType_User };
Result nifmInitialize(int type);
Result nifmGetCurrentIpAddress(u32 *ip);
