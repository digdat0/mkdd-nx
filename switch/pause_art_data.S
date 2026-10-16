/* Pause-screen art (tools/make_pause_art.py): the Switch picture, 352x256 RGBA, and the button
 * badges, 6x3 cells of 64x64 RGBA; top row first. */
    .section .rodata.pause_art,"a"
    .balign 16
    .global pause_console_rgba
pause_console_rgba:
    .incbin "pause_console.rgba"
    .balign 16
    .global pause_badges_rgba
pause_badges_rgba:
    .incbin "pause_badges.rgba"
