/* The pause screen's Switch picture and button badges (as switch/pause_art_data.S) and the Trick Style pack
 * (assets/trick.pak, tools/trickpak.py), for MinGW */
    .section .rdata,"dr"
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
