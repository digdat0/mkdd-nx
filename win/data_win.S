/* The pause screen's Switch picture and button badges (as switch/pause_art_data.S), for MinGW */
    .section .rdata,"dr"
    .balign 16
    .global pause_console_rgba
pause_console_rgba:
    .incbin "pause_console.rgba"
    .balign 16
    .global pause_badges_rgba
pause_badges_rgba:
    .incbin "pause_badges.rgba"
