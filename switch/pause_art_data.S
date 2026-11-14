/* Pause-screen art (tools/make_pause_art.py): the Switch picture, 352x256 RGBA, and the button
 * badges, 6x3 cells of 64x64 RGBA; top row first. Also the Trick Style pack (assets/trick.pak,
 * tools/trickpak.py), unpacked by runtime/trickpak.c. */
    .section .rodata.pause_art,"a"
    .balign 16
    .global pause_console_rgba
pause_console_rgba:
    .incbin "pause_console.rgba"
    .balign 16
    .global pause_badges_rgba
pause_badges_rgba:
    .incbin "pause_badges.rgba"
    .balign 16
    .global trick_pak
trick_pak:
    .incbin "trick.pak"
    .global trick_pak_end
trick_pak_end:
