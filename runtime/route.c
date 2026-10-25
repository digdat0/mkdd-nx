/* Battle routes (the - menu's SINGLE PLAYER BATTLE MODE, on the MODS page). The arenas have no route
 * for the computer's karts (their BOLs have no CL points), so they would sit still. A battle there gets
 * routes/<course ID>_<BOL hash>.txt (tools/battle_routes.py, tools/route_editor.py) as its CL points
 * (battle.c, loadCrsData): the hash tells a mod's arena from the game's of the same ID. The files are
 * the port's own data. Every arena's (the game's and MKDD Plus's) is also built in (route_builtin,
 * switch/CMakeLists.txt): a file on the SD card wins, else the built-in one is read the same way. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "plat.h"
#include "route.h"
#include "rt.h"

#define ROUTE_DIR "routes"
static char s_key[32];

static void route_path(char *out, size_t n) { snprintf(out, n, ROUTE_DIR "/%s.txt", s_key); }

/* the arena's route file: routes/<key>.txt, else the built-in one (path: which, for the log) */
static FILE *route_open(char *path, size_t n) {
    route_path(path, n);
    FILE *f = fopen(path, "r");
    if (f) return f;
    for (int i = 0; route_builtin[i].key; i++)
        if (!strcmp(route_builtin[i].key, s_key) && route_builtin[i].end > route_builtin[i].start) {
            snprintf(path, n, "built-in %s", s_key);
            return fmemopen((void *)route_builtin[i].start, (size_t)(route_builtin[i].end - route_builtin[i].start), "r");
        }
    return NULL;
}

void route_course(uint32_t id, uint32_t hash, bool battle) {
    (void)battle;
    snprintf(s_key, sizeof s_key, "%02x_%08x", (unsigned)id & 0xff, (unsigned)hash);
}

/* a line of just "-" (a coordinate may start with one), or of just "=" (between networks) */
static bool loop_break(const char *l) { return l[0] == '-' && (l[1] == 0 || l[1] == '\n' || l[1] == '\r'); }
static bool net_break(const char *l) { return l[0] == '=' && (l[1] == 0 || l[1] == '\n' || l[1] == '\r'); }

/* a line of the file (what is past the buffer dropped: a long comment is not read as more lines) */
static bool get_line(char *line, int size, FILE *f) {
    if (!fgets(line, size, f)) return false;
    if (!strchr(line, '\n')) {
        int ch;
        while ((ch = fgetc(f)) != EOF && ch != '\n') {}
    }
    return true;
}

#define NET_JUNCTIONS 64
#define NET_FORK 4 /* a CL point's next (and previous) points: CLPoint holds 4 */

/* a network (tools/battle_routes.py, tools/route_editor.py): "> a b" starts a part from junction a
 * to junction b, its points after. Each part is a CL group: its first point carries a's ID, its last
 * b's, and the game joins each part's last point to every part leaving b; a computer's kart takes
 * one of those at random each time (FUN_80226f1c: the next points not marked as a shortcut). The
 * game's own rules: junction 0 has one part leaving it (CLPoint 0's group), at most NET_FORK parts
 * leave or reach any junction. Anything else: no route. Read from where f is to the next "=" line
 * (a file may hold several networks). */
static int net_load(FILE *f, float (*pts)[3], int16_t *group, int max) {
    char line[96];
    int n = 0, parts = 0, first = -1, to = -1, out[NET_JUNCTIONS] = {0}, in[NET_JUNCTIONS] = {0};
    const char *bad = NULL;
    while (!bad && get_line(line, sizeof line, f)) {
        int a, b;
        float x, y, z;
        if (net_break(line)) break;
        if (line[0] == '>') {
            if (first >= 0 && n - first < 2) bad = "a part with fewer than 2 points";
            else if (sscanf(line + 1, "%d %d", &a, &b) != 2 || a < 0 || b < 0 || a >= NET_JUNCTIONS || b >= NET_JUNCTIONS) bad = "a bad part line";
            else if (++out[a] > NET_FORK || ++in[b] > NET_FORK) bad = "more than 4 parts leave or reach a junction";
            else {
                if (first >= 0) group[n - 1] = (int16_t)to;
                first = n, to = b, parts++;
                if (n < max) group[n] = (int16_t)a;
            }
            continue;
        }
        if (first < 0 || line[0] == '#' || sscanf(line, "%f %f %f", &x, &y, &z) != 3 || !isfinite(x) || !isfinite(y) || !isfinite(z)) continue;
        if (n == max) bad = "too many points";
        else {
            pts[n][0] = x, pts[n][1] = y, pts[n][2] = z;
            if (n != first) group[n] = -1;
            n++;
        }
    }
    if (!bad && first >= 0 && n - first < 2) bad = "a part with fewer than 2 points";
    if (!bad && first >= 0) group[n - 1] = (int16_t)to;
    if (!bad && out[0] != 1) bad = "junction 0 needs exactly one part leaving it";
    for (int j = 0; !bad && j < NET_JUNCTIONS; j++)
        if (!out[j] != !in[j]) bad = "a junction parts reach but none leave (or the other way)";
    if (!bad && !parts) bad = "no parts";
    if (bad) {
        rt_log("route: network not used: %s", bad);
        return 0;
    }
    rt_log("route: network, %d parts, %d points", parts, n);
    return n;
}

/* the arena's route file: networks (above; several: "=" between them), or loops ("-" between). Of
 * networks, up to want different ones in a random order each battle (one for each computer's kart;
 * one the game would refuse: the next instead), one after another in pts: network k from start[k]
 * (start[*nets] the end), its junctions numbered NET_JUNCTIONS * k + j so the networks never join
 * (only junction 0, network 0's, is special to the game). Of loops, one at random. */
int route_load(float (*pts)[3], int16_t *group, int max, int want, int *start, int *got) {
    char path[64], line[96];
    *got = 0, start[0] = 0;
    FILE *f = s_key[0] ? route_open(path, sizeof path) : NULL;
    if (!f) return 0;
    int loops = 1, nets = 1;
    bool net = false;
    while (get_line(line, sizeof line, f)) {
        if (loop_break(line)) loops++;
        if (net_break(line)) nets++;
        if (line[0] == '>') net = true;
    }
    if (net) {
        int order[ROUTE_FILE_NETS], count = nets < ROUTE_FILE_NETS ? nets : ROUTE_FILE_NETS, n = 0;
        uint64_t r = plat_now_ns() / 1000 | 1;
        for (int k = 0; k < count; k++) order[k] = k;
        for (int k = count - 1; k > 0; k--) { /* shuffled (xorshift from the clock) */
            r ^= r << 13, r ^= r >> 7, r ^= r << 17;
            int j = (int)(r % (uint64_t)(k + 1)), t = order[k];
            order[k] = order[j], order[j] = t;
        }
        char list[96] = "";
        for (int k = 0; k < count && *got < want && *got < ROUTE_NETS; k++) {
            int at = 0, room = max - n < ROUTE_MAX ? max - n : ROUTE_MAX;
            rewind(f);
            while (at < order[k] && get_line(line, sizeof line, f))
                if (net_break(line)) at++;
            int m = room >= 2 ? net_load(f, pts + n, group + n, room) : 0;
            if (!m) continue;
            for (int i = n; i < n + m; i++)
                if (group[i] >= 0) group[i] = (int16_t)(group[i] + NET_JUNCTIONS * *got);
            n += m, start[++*got] = n;
            size_t l = strlen(list);
            snprintf(list + l, sizeof list - l, "%s%d", l ? ", " : "", order[k] + 1);
        }
        fclose(f);
        if (n) rt_log("route: %s, network %s of %d", path, list, nets);
        return n;
    }
    int pick = (int)(plat_now_ns() / 1000 % (uint64_t)loops), at = 0, n = 0;
    rewind(f);
    while (n < max && get_line(line, sizeof line, f)) {
        float x, y, z;
        if (loop_break(line)) {
            if (at++ == pick) break;
            continue;
        }
        if (at != pick || line[0] == '#' || sscanf(line, "%f %f %f", &x, &y, &z) != 3 || !isfinite(x) || !isfinite(y) || !isfinite(z)) continue;
        pts[n][0] = x, pts[n][1] = y, pts[n][2] = z;
        n++;
    }
    fclose(f);
    rt_log("route: %s, loop %d of %d, %d points", path, pick + 1, loops, n);
    for (int i = 0; i < n; i++) group[i] = i == 0 || i == n - 1 ? 0 : -1; /* one group, from junction 0 back to it */
    if (n < 3) return 0;
    *got = 1, start[1] = n;
    return n;
}
