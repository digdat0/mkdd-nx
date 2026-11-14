"""The ten Trick Style tracks added for the cups (2026-11-12), in tracks7's terms (imported at the end of tracks7.py).
Every one is built onto Cookie Land's slot (build7.py <out> <track> <stem>), so any slot rules don't apply."""
from tracks7 import (TRACKS, rounded, at, gap2, spine, table2, rainbow, corner_berms, berm, hill, hills, containers)


def _start(L, s=1000): return (at(L, s)[0], at(L, s)[1], L[2])


import tracks7 as _t7
def course(legs_feats, margin=1500, items=4):
    """tracks7.course, but a leg too short for its features drops the last ones (printed)"""
    keep = []
    for leg, feats in legs_feats:
        feats = list(feats)
        while feats:
            ext = [_t7._extent(F) for F in feats]
            if _t7.leglen(leg) - 2 * margin - sum(b - a for a, b in ext) > 1200 * (len(feats) + 1): break
            print('tracks8: leg %.0f long drops %s' % (_t7.leglen(leg), feats[-1]['name']))
            feats.pop()
        keep.append((leg, feats))
    return _t7.course([(l, f) for l, f in keep if f], margin, items)


# =====================================================================================================
# MEADOW MOTOPLEX (Kicker Cup): green countryside, an L-shaped loop, friendly mid-size jumps
# =====================================================================================================
C = [(-30000, 16000), (24000, 16000), (24000, 2000), (2000, 2000), (2000, -16000), (-30000, -16000)]
RAD = [7000, 6000, 5000, 5000, 6000, 7000]
R, LG = rounded(C, RAD)
W_ = 6400
F, I, Cn = course([
    (LG[0], [gap2(1500, 2300, 1800, W_, mat='mound', name='meadow gap'), spine(1700, 2500, 400, W_, mat='mound', name='hay spine'),
             table2(1300, 2000, 2600, W_, name='farm table')]),
    (LG[1], [rainbow(1400, 2100, 1000, W_, name='brook')]),
    (LG[2], [gap2(1600, 2300, 1800, W_, shape='straight', name='fence gap')]),
    (LG[3], [spine(1500, 2200, 300, W_, mat='mound', name='barn spine')]),
    (LG[4], [table2(1400, 2100, 2400, W_, mat='mound', name='long table'), gap2(1900, 2700, 2100, W_, name='BIG MEADOW')]),
    (LG[5], [rainbow(1500, 2200, 1200, W_, mat='mound', name='hill hop')]),
], margin=1200, items=3)
F += corner_berms(C, RAD, 6000, 600)
TRACKS['meadow'] = dict(
    slot='mini7', title='MEADOW MOTOPLEX', arena=(-38000, -24000, 32000, 24000, 8000), cell=2600,
    height=hills(hill(10000, -9000, 6000, 5000, 1000), hill(-14000, 0, 6000, 4500, 700)),
    theme=dict(ground=((96, 162, 64), (70, 128, 48)), pebble=[(120, 176, 80), (84, 140, 60)], grass=[(126, 190, 86), (76, 136, 52)], road='clay',
               roadcol=(170, 124, 80), berm=(130, 98, 62), mound=(160, 118, 72), kick='wood', bar=((245, 245, 240), (220, 60, 40)), decor='fence',
               fencecol=(240, 240, 235), sky=((80, 150, 235), (215, 236, 255)), cloud=(255, 255, 255), sil=('hills', (80, 140, 90))),
    route=R, features=F, road_w=5800, items=I, coins=Cn, start=_start(LG[0], 900), cam=(-4000, 12000, 40000, -4000, 0, 0))

# =====================================================================================================
# CORAL COVE (Kicker Cup): beach sand, turquoise kickers, a wide triangle loop by the sea
# =====================================================================================================
C = [(-32000, 14000), (30000, 18000), (14000, -18000), (-26000, -14000)]
RAD = [8000, 8000, 7000, 7000]
R, LG = rounded(C, RAD)
W_ = 6600
F, I, Cn = course([
    (LG[0], [table2(1300, 2000, 2400, W_, mat='kick', name='boardwalk'), gap2(1700, 2500, 2000, W_, name='surf gap'),
             spine(1900, 2800, 400, W_, mat='mound', name='dune spine'), gap2(2000, 2800, 2200, W_, mat='mound', name='WAVE GAP')]),
    (LG[1], [rainbow(1600, 2400, 1200, W_, mat='mound', name='sandbar'), gap2(1600, 2300, 1900, W_, shape='straight', name='pier gap')]),
    (LG[2], [spine(1700, 2500, 300, W_, name='lifeguard'), table2(1400, 2200, 2600, W_, mat='mound', name='dune table')]),
    (LG[3], [gap2(1600, 2300, 1800, W_, mat='mound', name='cove gap')]),
], margin=1300, items=4)
F += corner_berms(C, RAD, 6400, 650)
TRACKS['coral'] = dict(
    slot='mini7', title='CORAL COVE', arena=(-40000, -26000, 38000, 26000, 8000), cell=2600,
    theme=dict(ground=((236, 214, 160), (210, 186, 130)), pebble=[(246, 230, 190), (200, 170, 120), (240, 200, 180)], pebbles=70, road='clay',
               roadcol=(214, 176, 112), berm=(200, 160, 100), mound=(222, 186, 124), kick='wood', kickcol=(40, 190, 190),
               bar=((250, 250, 250), (20, 170, 180)), decor='fence', fencecol=(150, 110, 70),
               sky=((40, 150, 230), (200, 240, 255)), cloud=(255, 255, 255), sil=('hills', (40, 120, 140))),
    route=R, features=F, road_w=6000, items=I, coins=Cn, start=_start(LG[0], 1000), cam=(0, 11000, 44000, 0, 0, 0))

# =====================================================================================================
# REDROCK RALLY (Canyon Cup): a red sandstone hexagon, step-down gaps, big dirt rainbows
# =====================================================================================================
C = [(-31000, 4000), (-16000, 19000), (16000, 19000), (31000, 2000), (16000, -18000), (-16000, -18000)]
RAD = [6000, 7000, 7000, 6000, 7000, 7000]
R, LG = rounded(C, RAD)
W_ = 6400
F, I, Cn = course([
    (LG[0], [gap2(1500, 2200, 1800, W_, mat='mound', name='rock hop')]),
    (LG[1], [gap2(1800, 2600, 2000, W_, name='arch gap'), spine(2000, 2900, 400, W_, mat='mound', name='butte spine'),
             table2(1300, 2100, 2200, W_, name='ledge')]),
    (LG[2], [rainbow(1700, 2500, 1200, W_, mat='mound', name='red rainbow')]),
    (LG[3], [spine(1800, 2600, 300, W_, shape='straight', name='needle')]),
    (LG[4], [table2(1500, 2300, 2800, W_, mat='mound', name='plateau'), gap2(2300, 3100, 2400, W_ + 800, name='REDROCK GAP'),
             rainbow(1500, 2200, 1000, W_, name='wash')]),
    (LG[5], [gap2(1700, 2400, 1900, W_, mat='mound', shape='straight', name='slot gap')]),
], margin=1200, items=3)
F += corner_berms(C, RAD, 6000, 650)
TRACKS['redrock'] = dict(
    slot='mini7', title='REDROCK RALLY', arena=(-39000, -26000, 39000, 27000, 9000), cell=2600,
    height=hills(hill(0, 1000, 9000, 6000, 1200), hill(-25000, -14000, 5000, 4000, 800), hill(26000, 14000, 5000, 4000, 900)),
    theme=dict(ground=((196, 104, 64), (150, 72, 44)), pebble=[(214, 130, 90), (120, 60, 40), (190, 110, 70)], road='clay', roadcol=(176, 96, 60),
               berm=(150, 80, 50), mound=(184, 100, 62), kick='wood', kickcol=(240, 180, 60), bar=((240, 236, 226), (170, 50, 30)),
               decor='fence', fencecol=(90, 56, 36), sky=((70, 140, 220), (255, 206, 150)), cloud=(255, 236, 216), sil=('mesa', (150, 70, 46))),
    route=R, features=F, road_w=5800, items=I, coins=Cn, start=_start(LG[1], 900), cam=(0, 12000, 44000, 0, 0, 0))

# =====================================================================================================
# SUNBURST SPEEDWAY (Canyon Cup): a fast desert oval, long tables and a mega gap on each straight
# =====================================================================================================
C = [(-34000, 13000), (34000, 13000), (34000, -13000), (-34000, -13000)]
RAD = [11000] * 4
R, LG = rounded(C, RAD)
W_ = 7000
F, I, Cn = course([
    (LG[0], [table2(1400, 2200, 3000, W_, mat='kick', name='sun table'), gap2(2000, 2800, 2200, W_, name='heat gap'),
             gap2(2500, 3300, 2500, W_ + 800, name='SUNBURST GAP')]),
    (LG[1], [spine(1600, 2300, 300, W_, mat='mound', name='east spine')]),
    (LG[2], [rainbow(1800, 2700, 1400, W_, name='mirage'), table2(1500, 2300, 3200, W_, mat='mound', name='long table'),
             spine(2100, 3000, 500, W_, name='SKY SPINE')]),
    (LG[3], [gap2(1700, 2400, 1900, W_, mat='mound', name='west gap')]),
], margin=1500, items=4)
F += corner_berms(C, RAD, 6800, 750)
TRACKS['sunburst'] = dict(
    slot='mini7', title='SUNBURST SPEEDWAY', arena=(-42000, -23000, 42000, 23000, 8000), cell=2600,
    theme=dict(ground=((226, 186, 120), (190, 148, 90)), pebble=[(240, 210, 150), (170, 130, 80)], road='asphalt', roadcol=(92, 84, 80),
               berm=(180, 140, 90), mound=(204, 160, 100), kick='steel', kickcol=(240, 140, 40), bar=((250, 246, 230), (240, 120, 20)),
               bartext='SUNBURST', decor='stands', track_fence='barrier', fence_h=300,
               sky=((250, 170, 70), (255, 230, 170)), cloud=(255, 220, 180), sil=('mesa', (190, 110, 70))),
    route=R, features=F, road_w=6200, berm=500, fence_off=3600, items=I, coins=Cn, start=_start(LG[0], 1200), cam=(0, 22000, 36000, 0, 0, 0))

# =====================================================================================================
# VOLCANO VENT (Canyon Cup): black basalt under a red sky, glowing orange steel kickers
# =====================================================================================================
C = [(-30000, 17000), (6000, 21000), (31000, 8000), (24000, -18000), (-4000, -12000), (-30000, -18000)]
RAD = [7000, 8000, 7000, 7000, 6000, 7000]
R, LG = rounded(C, RAD)
W_ = 6400
F, I, Cn = course([
    (LG[0], [gap2(1800, 2600, 2000, W_, name='ember gap'), spine(2100, 3000, 400, W_, mat='mound', name='ash spine')]),
    (LG[1], [table2(1400, 2100, 2400, W_, name='lava table'), gap2(1700, 2400, 1900, W_, mat='mound', shape='straight', name='vent gap')]),
    (LG[2], [rainbow(1900, 2700, 1300, W_, name='CALDERA')]),
    (LG[3], [spine(1700, 2400, 300, W_, shape='straight', name='crater spine'), gap2(1600, 2300, 1800, W_, mat='mound', name='flow gap')]),
    (LG[4], [gap2(2300, 3100, 2400, W_ + 800, name='ERUPTION')]),
    (LG[5], [table2(1300, 2000, 2200, W_, mat='mound', name='cinder table')]),
], margin=1200, items=3)
F += corner_berms(C, RAD, 6000, 650)
TRACKS['volcano'] = dict(
    slot='mini7', title='VOLCANO VENT', arena=(-39000, -26000, 39000, 29000, 9000), cell=2600,
    height=hills(hill(2000, 3000, 8000, 6000, 1400), hill(-22000, -2000, 4000, 5000, 700)),
    theme=dict(ground=((70, 58, 56), (44, 36, 36)), pebble=[(96, 80, 76), (30, 26, 26), (150, 60, 30)], road='clay', roadcol=(96, 70, 60),
               berm=(80, 60, 54), mound=(104, 80, 70), kick='steel', kickcol=(240, 100, 30), bar=((40, 36, 36), (240, 90, 20)),
               decor='fence', fencecol=(40, 34, 34), sky=((60, 16, 20), (230, 100, 50)), cloud=(160, 90, 80), sil=('mesa', (40, 26, 26))),
    route=R, features=F, road_w=5800, items=I, coins=Cn, start=_start(LG[0], 900), cam=(0, 12000, 46000, 0, 0, 0))

# =====================================================================================================
# STEELWORKS (City Cup): a factory yard: steel ramps, container stacks, smoky orange sky
# =====================================================================================================
C = [(-32000, 15000), (30000, 15000), (30000, -2000), (6000, -2000), (6000, -17000), (-32000, -17000)]
RAD = [6000, 6000, 5000, 5000, 6000, 6000]
R, LG = rounded(C, RAD)
W_ = 6000
F, I, Cn = course([
    (LG[0], [gap2(1700, 2400, 2000, W_, name='crane gap'), table2(1300, 2000, 2600, W_, mat='kick', name='loading dock'),
             spine(1900, 2700, 400, W_, name='girder spine'), gap2(2200, 3000, 2300, W_ + 600, name='FURNACE GAP')]),
    (LG[1], [spine(1500, 2100, 300, W_, shape='straight', name='pipe hop')]),
    (LG[2], [gap2(1600, 2200, 1800, W_, shape='straight', name='rail gap')]),
    (LG[3], [table2(1300, 1900, 2200, W_, mat='kick', name='forklift')]),
    (LG[4], [rainbow(1700, 2500, 1200, W_, mat='kick', name='conveyor'), gap2(1900, 2700, 2100, W_, name='smelter gap')]),
    (LG[5], [spine(1700, 2400, 300, W_, name='yard spine')]),
], margin=1100, items=3)
TRACKS['steelworks'] = dict(
    slot='mini7', title='STEELWORKS', arena=(-40000, -25000, 38000, 23000, 8000), cell=2600,
    theme=dict(ground=((120, 112, 104), (90, 84, 78)), pebble=[(140, 130, 120), (70, 64, 60)], pebbles=60, road='asphalt', roadcol=(66, 66, 70),
               berm=(110, 104, 100), mound=(140, 134, 128), kick='steel', kickcol=(230, 180, 40), bar=((40, 40, 44), (240, 190, 30)),
               decor='city', track_fence='barrier', fence_h=360, concrete=(140, 132, 124),
               sky=((110, 90, 80), (240, 160, 100)), cloud=(200, 170, 150), sil=('skyline', (70, 60, 56))),
    route=R, features=F, road_w=5400, berm=500, fence_off=3100, items=I, coins=Cn, start=_start(LG[0], 900), cam=(-2000, 12000, 21000, 0, 1000, -4000))
TRACKS['steelworks']['boxes'] = containers(TRACKS['steelworks']['route'], TRACKS['steelworks']['arena'], 6500, 11)

# =====================================================================================================
# NEON NIGHTS (City Cup): a city street loop at night, magenta and cyan steel, a skyline of lights
# =====================================================================================================
C = [(-33000, 12000), (-12000, 19000), (12000, 12000), (32000, 18000), (32000, -16000), (-33000, -16000)]
RAD = [6000, 7000, 6000, 6000, 7000, 7000]
R, LG = rounded(C, RAD)
W_ = 6000
F, I, Cn = course([
    (LG[0], [gap2(1600, 2300, 1900, W_, name='neon gap')]),
    (LG[1], [spine(1800, 2500, 400, W_, name='billboard')]),
    (LG[2], [table2(1300, 2000, 2200, W_, mat='kick', name='plaza table')]),
    (LG[3], [gap2(1900, 2600, 2100, W_, shape='straight', name='tower gap'), rainbow(1600, 2300, 1100, W_, mat='kick', name='skybridge')]),
    (LG[4], [table2(1500, 2300, 3000, W_, mat='kick', name='rooftop'), gap2(2300, 3100, 2400, W_ + 600, name='NEON DROP'),
             spine(1900, 2700, 300, W_, name='laser spine'), gap2(1700, 2400, 1900, W_, name='avenue gap')]),
    (LG[5], [rainbow(1700, 2400, 1200, W_, name='overpass')]),
], margin=1100, items=3)
TRACKS['neon'] = dict(
    slot='mini7', title='NEON NIGHTS', arena=(-41000, -24000, 40000, 27000, 8000), cell=2600,
    theme=dict(ground=((60, 56, 76), (40, 38, 54)), pebble=[(80, 76, 100), (30, 28, 40)], pebbles=60, road='asphalt', roadcol=(48, 46, 60),
               berm=(70, 66, 90), mound=(90, 86, 110), kick='steel', kickcol=(220, 60, 200), bar=((30, 230, 240), (230, 40, 200)),
               decor='city', track_fence='barrier', fence_h=360, concrete=(96, 92, 120),
               sky=((6, 6, 26), (70, 30, 100)), stars=True, nofog=True, sil=('skyline', (24, 18, 50))),
    route=R, features=F, road_w=5400, berm=500, fence_off=3100, items=I, coins=Cn, start=_start(LG[4], 1000), cam=(0, 16000, 40000, 0, 0, 0))

# =====================================================================================================
# GLACIER GAP (Summit Cup): ice field, the longest ice kickers, crevasse gaps
# =====================================================================================================
C = [(-31000, 18000), (31000, 14000), (24000, -4000), (31000, -18000), (-31000, -16000)]
RAD = [8000, 6000, 5000, 6000, 8000]
R, LG = rounded(C, RAD)
W_ = 7000
F, I, Cn = course([
    (LG[0], [gap2(2000, 2900, 2200, W_, name='crevasse'), spine(2400, 3300, 500, W_, name='SERAC'),
             gap2(1800, 2600, 2000, W_, shape='straight', name='ice gap')]),
    (LG[1], [table2(1300, 1900, 1800, W_, mat='kick', name='shelf')]),
    (LG[2], [spine(1500, 2100, 300, W_, shape='straight', name='nub')]),
    (LG[3], [gap2(2600, 3600, 2500, W_ + 1000, name='GLACIER GAP'), rainbow(1800, 2700, 1300, W_, mat='kick', name='floe')]),
    (LG[4], [table2(1500, 2200, 2400, W_, mat='kick', name='west shelf')]),
], margin=1300, items=4)
F += corner_berms(C, RAD, 6600, 700)
TRACKS['glacier'] = dict(
    slot='mini7', title='GLACIER GAP', arena=(-39000, -26000, 39000, 28000, 9000), cell=2600,
    height=hills(hill(-2000, 0, 9000, 6000, 1100), hill(-24000, -6000, 4000, 5000, 800)),
    theme=dict(ground=((220, 236, 250), (176, 204, 230)), pebble=[(240, 248, 255), (150, 190, 226)], pebbles=90, road='clay', roadcol=(196, 216, 238),
               berm=(186, 208, 232), mound=(214, 230, 248), snow=True, kick='ice', kickcol=(70, 170, 240), bar=((240, 248, 255), (20, 90, 200)),
               decor='fence', fencecol=(40, 70, 120), sky=((40, 110, 200), (200, 226, 250)), sil=('mountains', (100, 130, 180))),
    route=R, features=F, road_w=6200, items=I, coins=Cn, start=_start(LG[0], 1000), cam=(0, 12000, 44000, 0, 0, 0))

# =====================================================================================================
# ALPINE SUMMIT (Summit Cup): high meadows between peaks, rolling hills and wooden kickers
# =====================================================================================================
C = [(-32000, 10000), (-20000, 20000), (8000, 14000), (32000, 19000), (30000, -8000), (8000, -18000), (-30000, -16000)]
RAD = [6000, 6000, 7000, 6000, 7000, 7000, 7000]
R, LG = rounded(C, RAD)
W_ = 6200
F, I, Cn = course([
    (LG[0], [rainbow(1400, 2000, 900, W_, mat='mound', name='alm hop')]),
    (LG[1], [gap2(1700, 2400, 1900, W_, name='chalet gap'), spine(1800, 2500, 300, W_, mat='mound', name='ridge')]),
    (LG[2], [table2(1300, 1900, 2000, W_, name='cowbell')]),
    (LG[3], [gap2(2200, 3000, 2300, W_ + 600, name='SUMMIT GAP'), rainbow(1600, 2300, 1100, W_, name='pass')]),
    (LG[4], [spine(1900, 2700, 400, W_, mat='mound', name='scree spine'), table2(1400, 2100, 2400, W_, mat='mound', name='pasture')]),
    (LG[5], [gap2(1800, 2600, 2000, W_, mat='mound', shape='straight', name='col gap'), spine(1600, 2300, 300, W_, name='cairn')]),
    (LG[6], [gap2(1600, 2300, 1800, W_, mat='mound', name='valley gap')]),
], margin=1100, items=3)
F += corner_berms(C, RAD, 6000, 650)
TRACKS['alpine'] = dict(
    slot='mini7', title='ALPINE SUMMIT', arena=(-40000, -26000, 40000, 28000, 9000), cell=2600,
    height=hills(hill(-6000, 3000, 9000, 6000, 1500), hill(20000, 3000, 6000, 6000, 1100), hill(-26000, -4000, 4000, 6000, 800)),
    theme=dict(ground=((110, 160, 80), (76, 120, 56)), pebble=[(140, 180, 100), (130, 126, 120), (90, 88, 84)], grass=[(130, 186, 96), (80, 130, 60)],
               road='clay', roadcol=(160, 130, 96), berm=(120, 100, 76), mound=(156, 128, 92), kick='wood', bar=((245, 245, 245), (200, 30, 40)),
               decor='fence', fencecol=(110, 76, 50), sky=((60, 130, 230), (210, 230, 255)), cloud=(255, 255, 255), sil=('mountains', (110, 120, 150))),
    route=R, features=F, road_w=5800, items=I, coins=Cn, start=_start(LG[1], 900), cam=(0, 13000, 46000, 0, 0, 0), key_salt=5)

# =====================================================================================================
# MEGA RAMP ARENA (Summit Cup finale): a night stadium, the biggest jumps of all
# =====================================================================================================
C = [(-32000, 16000), (32000, 16000), (32000, -16000), (-32000, -16000)]
RAD = [9000] * 4
R, LG = rounded(C, RAD)
W_ = 7400
F, I, Cn = course([
    (LG[0], [gap2(2200, 3000, 2300, W_, name='warm-up'), gap2(2800, 3800, 2600, W_ + 1200, name='MEGA RAMP'),
             spine(2400, 3300, 500, W_, name='tower spine')]),
    (LG[1], [table2(1500, 2300, 2400, W_, mat='kick', name='east deck')]),
    (LG[2], [rainbow(2200, 3200, 1500, W_, mat='kick', name='SKY BRIDGE'), gap2(2600, 3600, 2500, W_ + 1000, name='HYPER GAP'),
             table2(1400, 2200, 2400, W_, mat='kick', name='cool-down')]),
    (LG[3], [spine(2000, 2800, 300, W_, name='west spine')]),
], margin=1300, items=4)
F += corner_berms(C, RAD, 7000, 800)
TRACKS['megaramp'] = dict(
    slot='mini7', title='MEGA RAMP ARENA', arena=(-40000, -24000, 40000, 24000, 8000), cell=2600,
    theme=dict(ground=((60, 110, 60), (40, 80, 40)), pebble=[(80, 130, 70), (40, 70, 40)], pebbles=60, grass=[(80, 140, 70), (40, 90, 40)],
               road='clay', roadcol=(180, 110, 64), berm=(150, 90, 54), mound=(176, 110, 66), kick='steel', kickcol=(240, 40, 60),
               bar=((250, 250, 250), (220, 30, 50)), bartext='MEGA RAMP', decor='stands', track_fence='barrier', fence_h=300,
               sky=((10, 14, 40), (50, 60, 110)), stars=True, nofog=True),
    route=R, features=F, road_w=6400, berm=500, fence_off=3800, items=I, coins=Cn, start=_start(LG[0], 1200), cam=(0, 24000, 36000, 0, 0, 0),
    key_salt=7)
