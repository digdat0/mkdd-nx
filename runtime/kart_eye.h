/* KART FPV's eye for each stock kart, by EKartID: above and ahead of the kart's centre (KartBody
 * +0x110's position, along its up and forward axes), degrees down. The - menu's EYE HEIGHT, EYE FORWARD
 * and TILT, saved for each kart, add to these. Fitted to each body's shape and checked in screenshots (tools/fpv_test.sh). */
#pragma once
static const struct { short up, fwd, tilt; } KART_EYE[21] = {
    {206, -37, 10}, /* 0 Red Fire */
    {233, 0, 10}, /* 1 DK Jumbo */
    {208, -44, 10}, /* 2 Turbo Yoshi */
    {180, -35, 10}, /* 3 Koopa Dasher */
    {223, -37, 10}, /* 4 Heart Coach */
    {206, -25, 10}, /* 5 Goo-Goo Buggy */
    {196, 40, 10}, /* 6 Wario Car */
    {222, 14, 10}, /* 7 Koopa King */
    {206, -37, 10}, /* 8 Green Fire */
    {198, -30, 10}, /* 9 Barrel Train */
    {210, -15, 10}, /* 10 Turbo Birdo */
    {180, -35, 10}, /* 11 Para-Wing */
    {223, -37, 10}, /* 12 Bloom Coach */
    {206, -25, 10}, /* 13 Rattle Buggy */
    {183, -30, 10}, /* 14 Waluigi Racer */
    {206, -15, 10}, /* 15 Bullet Blaster */
    {173, -23, 10}, /* 16 Toad Kart */
    {173, -23, 10}, /* 17 Toadette Kart */
    {243, -30, 10}, /* 18 Boo Pipes */
    {243, -30, 10}, /* 19 Piranha Pipes */
    {163, -15, 10}, /* 20 Parade Kart */
};
