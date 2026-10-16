/* Loading-screen logo: 512x512 RGBA, top row first (tools/make_logo.py). */
    .section .rodata.mkdd_logo,"a"
    .balign 16
    .global mkdd_logo_rgba
mkdd_logo_rgba:
    .incbin "logo_512.rgba"
