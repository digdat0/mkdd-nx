/* In-game updater, see update.h. The release carries two assets: MKDD-NX.nro and update.txt
 *   version=1.2.0
 *   size=12345678
 *   sha256=<64 hex>
 * fetched from github.com/<repo>/releases/latest/download/ (no API, no token: the repository has
 * to be public while players update). An update is offered when the release's version is not
 * older than this build's and its .nro differs from the running one, so same-version rebuilds
 * install too and nothing ever downgrades. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <switch.h>
#include <curl/curl.h>

#include "update.h"
#include "../runtime/plat.h"
#include "../runtime/rt.h"

#ifndef MKDD_VERSION
#define MKDD_VERSION "0.0.0"
#endif
#define RELEASE_URL "https://github.com/digdat0/mkdd-nx/releases/latest/download/"
#define MANIFEST_MAX 4096

static PMutex s_lock;
static UpdateStatus s_st;
static bool s_busy; /* the worker is running */
static char s_nro[512];
static struct { char version[24]; unsigned long long size; u8 sha[32]; } s_rel;

const char *update_version(void) { return MKDD_VERSION; }

void update_init(const char *nro_path) {
    pmutex_init(&s_lock);
    size_t n = nro_path ? strlen(nro_path) : 0;
    if (n > 4 && n < sizeof s_nro && !strncmp(nro_path, "sdmc:/", 6) && !strcasecmp(nro_path + n - 4, ".nro"))
        snprintf(s_nro, sizeof s_nro, "%s", nro_path);
    else
        snprintf(s_nro, sizeof s_nro, "sdmc:/switch/MKDD-NX/MKDD-NX.nro");
}

void update_status(UpdateStatus *out) {
    pmutex_lock(&s_lock);
    *out = s_st;
    pmutex_unlock(&s_lock);
}

static void set_state(int state, const char *msg) {
    pmutex_lock(&s_lock);
    s_st.state = state;
    if (msg) snprintf(s_st.msg, sizeof s_st.msg, "%s", msg);
    pmutex_unlock(&s_lock);
    if (msg) rt_log("update: %s", msg);
}

/* "1.10.2" > "1.9.9": numeric parts, missing ones 0. */
static int version_cmp(const char *a, const char *b) {
    for (int i = 0; i < 4; i++) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* ---- HTTP ---- */
typedef struct {
    char *buf; size_t len, cap;   /* manifest: into memory */
    FILE *f; Sha256Context sha;   /* .nro: to the file, hashed on the way */
    unsigned long long total;
} Sink;

static size_t on_data(char *p, size_t sz, size_t n, void *u) {
    Sink *s = u;
    size_t len = sz * n;
    if (s->f) {
        if (fwrite(p, 1, len, s->f) != len) return 0;
        sha256ContextUpdate(&s->sha, p, len);
    } else {
        if (s->len + len >= s->cap) return 0;
        memcpy(s->buf + s->len, p, len);
        s->len += len;
        s->buf[s->len] = 0;
    }
    return len;
}

static int on_progress(void *u, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal; (void)ulnow;
    Sink *s = u;
    unsigned long long total = s->total ? s->total : (unsigned long long)dltotal;
    if (total) {
        pmutex_lock(&s_lock);
        s_st.progress = (float)((double)dlnow / (double)total);
        pmutex_unlock(&s_lock);
    }
    return 0;
}

/* GET url into the sink. Returns NULL or what went wrong, for the menu. */
static const char *fetch(const char *url, Sink *s) {
    CURL *h = curl_easy_init();
    if (!h) return "COULD NOT START THE DOWNLOAD";
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L); /* releases/latest/download -> the asset's CDN url */
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "MKDD-NX/" MKDD_VERSION);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 512L); /* stalled for 30 s: give up */
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, s);
    CURLcode r = curl_easy_perform(h);
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(h);
    rt_log("update: GET %s -> curl %d (%s), HTTP %ld", url, (int)r, curl_easy_strerror(r), code);
    if (r == CURLE_COULDNT_RESOLVE_HOST || r == CURLE_COULDNT_CONNECT) return "NO INTERNET CONNECTION";
    if (r == CURLE_OPERATION_TIMEDOUT) return "THE CONNECTION TIMED OUT: TRY AGAIN";
    if (r == CURLE_WRITE_ERROR) return s->f ? "COULD NOT WRITE TO THE SD CARD" : "UPDATE.TXT IS TOO BIG";
    if (r != CURLE_OK) return "DOWNLOAD FAILED: TRY AGAIN";
    if (code == 404) return "NO RELEASE FOUND (IS THE REPOSITORY PUBLIC?)";
    if (code != 200) return "THE SERVER REFUSED: TRY AGAIN LATER";
    return NULL;
}

/* ---- steps ---- */
static bool parse_manifest(const char *t) {
    memset(&s_rel, 0, sizeof s_rel);
    bool have_sha = false;
    for (const char *line = t; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (!strncmp(line, "version=", 8)) {
            size_t n = strcspn(line + 8, "\r\n");
            if (n >= sizeof s_rel.version) n = sizeof s_rel.version - 1;
            memcpy(s_rel.version, line + 8, n);
            s_rel.version[n] = 0;
        } else if (!strncmp(line, "size=", 5)) {
            s_rel.size = strtoull(line + 5, NULL, 10);
        } else if (!strncmp(line, "sha256=", 7)) {
            have_sha = true;
            for (int i = 0; i < 32; i++) {
                int hi = hexval(line[7 + 2 * i]), lo = hi < 0 ? -1 : hexval(line[8 + 2 * i]);
                if (lo < 0) { have_sha = false; break; }
                s_rel.sha[i] = (u8)(hi << 4 | lo);
            }
        }
    }
    return s_rel.version[0] && s_rel.size > 0x80 && s_rel.size < (256ull << 20) && have_sha;
}

/* SHA-256 of a file; false when it can't be read. */
static bool file_sha(const char *path, u8 out[32], unsigned long long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    static u8 buf[256 << 10];
    Sha256Context c;
    sha256ContextCreate(&c);
    unsigned long long n = 0;
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) { sha256ContextUpdate(&c, buf, r); n += r; }
    fclose(f);
    sha256ContextGetHash(&c, out);
    if (size) *size = n;
    return true;
}

static void do_check(void) {
    char *buf = calloc(1, MANIFEST_MAX);
    Sink s = {buf, 0, MANIFEST_MAX};
    const char *err = buf ? fetch(RELEASE_URL "update.txt", &s) : "OUT OF MEMORY";
    if (!err && !parse_manifest(buf)) err = "THE RELEASE HAS NO VALID UPDATE.TXT";
    free(buf);
    if (err) { set_state(UPD_ERROR, err); return; }
    pmutex_lock(&s_lock);
    snprintf(s_st.version, sizeof s_st.version, "%s", s_rel.version);
    pmutex_unlock(&s_lock);
    u8 cur[32];
    bool same = file_sha(s_nro, cur, NULL) && !memcmp(cur, s_rel.sha, 32);
    int vc = version_cmp(s_rel.version, MKDD_VERSION);
    rt_log("update: release %s (%llu bytes), running %s from %s: %s", s_rel.version, s_rel.size, MKDD_VERSION, s_nro,
           same ? "same file" : vc < 0 ? "older" : "different");
    set_state(same || vc < 0 ? UPD_CURRENT : UPD_AVAILABLE, NULL);
}

static void do_install(void) {
    char tmp[sizeof s_nro + 8], old[sizeof s_nro + 8];
    snprintf(tmp, sizeof tmp, "%s.new", s_nro);
    snprintf(old, sizeof old, "%s.old", s_nro);
    remove(tmp);
    Sink s = {0};
    s.total = s_rel.size;
    s.f = fopen(tmp, "wb");
    if (!s.f) { set_state(UPD_ERROR, "COULD NOT WRITE TO THE SD CARD"); return; }
    setvbuf(s.f, NULL, _IOFBF, 256 << 10);
    sha256ContextCreate(&s.sha);
    const char *err = fetch(RELEASE_URL "MKDD-NX.nro", &s);
    if (fclose(s.f) != 0 && !err) err = "COULD NOT WRITE TO THE SD CARD";
    u8 got[32];
    sha256ContextGetHash(&s.sha, got);
    /* Check what landed on the card, not only what came over the wire. */
    unsigned long long size = 0;
    u8 disk[32];
    if (!err && (!file_sha(tmp, disk, &size) || size != s_rel.size || memcmp(disk, s_rel.sha, 32) || memcmp(got, s_rel.sha, 32)))
        err = "THE DOWNLOAD IS DAMAGED: TRY AGAIN";
    if (!err) {
        FILE *f = fopen(tmp, "rb");
        char magic[4] = {0};
        if (!f || fseek(f, 0x10, SEEK_SET) || fread(magic, 1, 4, f) != 4 || memcmp(magic, "NRO0", 4)) err = "THE DOWNLOAD IS NOT A SWITCH APP";
        if (f) fclose(f);
    }
    if (err) { remove(tmp); set_state(UPD_ERROR, err); return; }
    /* Swap: running -> .old, new -> running (newlib's rename doesn't replace, so remove first). */
    remove(old);
    if (rename(s_nro, old) != 0) { remove(tmp); set_state(UPD_ERROR, "COULD NOT REPLACE THE APP FILE"); return; }
    if (rename(tmp, s_nro) != 0) {
        rename(old, s_nro);
        remove(tmp);
        set_state(UPD_ERROR, "COULD NOT REPLACE THE APP FILE");
        return;
    }
    rt_log("update: installed %s as %s (previous kept as %s)", s_rel.version, s_nro, old);
    set_state(UPD_READY, NULL);
}

static void worker(void *arg) {
    bool install = arg != NULL;
    /* sockets only while the updater works: they hold transfer memory the game doesn't need */
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        rt_log("update: socketInitializeDefault %#x", rc);
        set_state(UPD_ERROR, "NETWORK UNAVAILABLE");
    } else {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        if (install) do_install();
        else do_check();
        curl_global_cleanup();
        socketExit();
    }
    pmutex_lock(&s_lock);
    s_busy = false;
    pmutex_unlock(&s_lock);
}

void update_start(void) {
    pmutex_lock(&s_lock);
    if (s_busy || s_st.state == UPD_READY) { pmutex_unlock(&s_lock); return; }
    bool install = s_st.state == UPD_AVAILABLE;
    s_busy = true;
    s_st.state = install ? UPD_DOWNLOADING : UPD_CHECKING;
    s_st.progress = 0;
    s_st.msg[0] = 0;
    pmutex_unlock(&s_lock);
    /* low priority on any core: never in the game's way */
    if (!pthread_start("mkdd-update", worker, install ? (void *)1 : NULL, 512u << 10, -1, 4)) {
        pmutex_lock(&s_lock);
        s_busy = false;
        pmutex_unlock(&s_lock);
        set_state(UPD_ERROR, "COULD NOT START THE UPDATER");
    }
}

bool update_relaunch(void) {
    if (!envHasNextLoad()) return false;
    return R_SUCCEEDED(envSetNextLoad(s_nro, s_nro));
}
