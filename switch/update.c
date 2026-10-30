/* In-game updater, see update.h. What the latest release is comes from GitHub's own record of it,
 * api.github.com/repos/<repo>/releases/latest: its tag (v1.2.0) and its MKDD-NX.nro asset's size,
 * SHA-256 (the asset's "digest", which GitHub works out on upload) and download url, so a release
 * needs nothing but the .nro. When the API can't be used (60 requests an hour per address without
 * a token, or an asset uploaded before GitHub kept digests) it falls back to the release's update.txt
 *   version=1.2.0
 *   size=12345678
 *   sha256=<64 hex>
 * the only thing builds up to 1.6.1 read (tools/release.sh still attaches it for them). No token
 * either way: the repository has to be public while players update. An update is offered when the
 * release's version is not older than this build's and its .nro differs from the running one, so
 * same-version rebuilds install too and nothing ever downgrades. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include <switch.h>
#include <curl/curl.h>

#include "update.h"
#include "../runtime/plat.h"
#include "../runtime/rt.h"

#if __has_include("mkdd_version.h")
#include "mkdd_version.h" /* MKDD_VERSION, written by CMakeLists.txt */
#endif
#ifndef MKDD_VERSION
#define MKDD_VERSION "0.0.0"
#endif
#define REPO "digdat0/mkdd-nx"
#define API_URL "https://api.github.com/repos/" REPO "/releases/latest"
#define RELEASE_URL "https://github.com/" REPO "/releases/latest/download/"
#define ASSET_URL "https://github.com/" REPO "/releases/download/" /* the API's download urls */
#define MANIFEST_MAX 4096
#define API_MAX (512 << 10) /* the release's JSON: its notes and every asset */

static PMutex s_lock;
static UpdateStatus s_st;
static bool s_busy; /* the worker is running */
static char s_nro[512];
static struct { char version[24]; unsigned long long size; u8 sha[32]; char url[384]; } s_rel;

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

/* 64 hex digits -> 32 bytes */
static bool parse_sha(const char *hex, u8 out[32]) {
    for (int i = 0; i < 32; i++) {
        int hi = hexval(hex[2 * i]), lo = hi < 0 ? -1 : hexval(hex[2 * i + 1]);
        if (lo < 0) return false;
        out[i] = (u8)(hi << 4 | lo);
    }
    return true;
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
    struct curl_slist *hdr = NULL;
    if (!strncmp(url, "https://api.github.com/", 23)) {
        hdr = curl_slist_append(hdr, "Accept: application/vnd.github+json");
        hdr = curl_slist_append(hdr, "X-GitHub-Api-Version: 2022-11-28");
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr);
    }
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
    curl_slist_free_all(hdr);
    rt_log("update: GET %s -> curl %d (%s), HTTP %ld", url, (int)r, curl_easy_strerror(r), code);
    if (r == CURLE_COULDNT_RESOLVE_HOST || r == CURLE_COULDNT_CONNECT) return "NO INTERNET CONNECTION";
    if (r == CURLE_OPERATION_TIMEDOUT) return "THE CONNECTION TIMED OUT: TRY AGAIN";
    if (r == CURLE_WRITE_ERROR) return s->f ? "COULD NOT WRITE TO THE SD CARD" : "THE RELEASE INFO IS TOO BIG";
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
            have_sha = parse_sha(line + 7, s_rel.sha);
        }
    }
    snprintf(s_rel.url, sizeof s_rel.url, "%s", RELEASE_URL "MKDD-NX.nro");
    return s_rel.version[0] && s_rel.size > 0x80 && s_rel.size < (256ull << 20) && have_sha;
}

/* ---- the API's JSON: just enough of it, never trusting its shape (NUL-terminated text) ---- */
static const char *ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Past the value at p (strings, nesting), NULL when it's cut off or malformed. */
static const char *skip(const char *p) {
    int depth = 0;
    p = ws(p);
    do {
        if (*p == '"') {
            for (p++; *p && *p != '"'; p++)
                if (*p == '\\' && p[1]) p++;
            if (!*p) return NULL;
            p++;
        } else if (*p == '{' || *p == '[') {
            if (++depth > 64) return NULL;
            p++;
        } else if (*p == '}' || *p == ']') {
            if (!depth--) return NULL;
            p++;
        } else if (!*p) {
            return NULL;
        } else {
            p += depth ? 1 : strcspn(p, ",}] \t\r\n"); /* a number, true, null */
        }
    } while (depth > 0);
    return p;
}

/* The value of the object at p's own member key (not a nested object's), NULL when absent. */
static const char *member(const char *p, const char *key) {
    size_t k = strlen(key);
    if (!p || *(p = ws(p)) != '{') return NULL;
    for (p++;;) {
        p = ws(p);
        if (*p != '"') return NULL;
        const char *name = p + 1, *v = skip(p);
        if (!v || *(v = ws(v)) != ':') return NULL;
        v = ws(v + 1);
        if (!strncmp(name, key, k) && name[k] == '"') return v;
        if (!(p = skip(v)) || *(p = ws(p)) != ',') return NULL;
        p++;
    }
}

/* A string value without escapes into out. */
static bool str_val(const char *v, char *out, size_t n) {
    if (!v || *v != '"') return false;
    size_t i = 0;
    for (v++; *v != '"'; v++) {
        if (!*v || *v == '\\' || i + 1 >= n) return false;
        out[i++] = *v;
    }
    out[i] = 0;
    return true;
}

/* releases/latest: the tag and MKDD-NX.nro's size, digest and url into s_rel. */
static bool parse_release(const char *t) {
    memset(&s_rel, 0, sizeof s_rel);
    char tag[32], name[64], digest[80];
    if (!str_val(member(t, "tag_name"), tag, sizeof tag)) return false;
    const char *v = tag + (tag[0] == 'v' || tag[0] == 'V');
    if (!isdigit((unsigned char)*v)) return false;
    snprintf(s_rel.version, sizeof s_rel.version, "%s", v);
    const char *a = member(t, "assets");
    if (!a || *a != '[') return false;
    for (a = ws(a + 1); *a == '{'; a = ws(a + 1)) {
        if (str_val(member(a, "name"), name, sizeof name) && !strcmp(name, "MKDD-NX.nro")) {
            if (!skip(a)) return false; /* cut off inside it: a number could be short */
            const char *size = member(a, "size");
            s_rel.size = size && isdigit((unsigned char)*size) ? strtoull(size, NULL, 10) : 0;
            return str_val(member(a, "digest"), digest, sizeof digest) && !strncmp(digest, "sha256:", 7) &&
                   strlen(digest) == 7 + 64 && parse_sha(digest + 7, s_rel.sha) &&
                   str_val(member(a, "browser_download_url"), s_rel.url, sizeof s_rel.url) &&
                   !strncmp(s_rel.url, ASSET_URL, strlen(ASSET_URL)) &&
                   s_rel.size > 0x80 && s_rel.size < (256ull << 20);
        }
        if (!(a = skip(a)) || *(a = ws(a)) != ',') break;
    }
    return false;
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

/* The latest release into s_rel: GitHub's record of it, else its update.txt. NULL or what went wrong. */
static const char *find_release(void) {
    char *buf = calloc(1, API_MAX);
    if (!buf) return "OUT OF MEMORY";
    Sink s = {buf, 0, API_MAX};
    const char *err = fetch(API_URL, &s);
    if (!err && !parse_release(buf)) err = "THE RELEASE HAS NO MKDD-NX.NRO";
    /* offline: update.txt can't help */
    if (err && strcmp(err, "NO INTERNET CONNECTION") && strcmp(err, "THE CONNECTION TIMED OUT: TRY AGAIN")) {
        rt_log("update: the API gave \"%s\", trying update.txt", err);
        memset(buf, 0, MANIFEST_MAX);
        s = (Sink){buf, 0, MANIFEST_MAX};
        const char *api = err;
        err = fetch(RELEASE_URL "update.txt", &s);
        if (!err && !parse_manifest(buf)) err = "THE RELEASE HAS NO VALID UPDATE.TXT";
        if (err) err = api; /* the API's answer says more */
    }
    free(buf);
    return err;
}

static void do_check(void) {
    const char *err = find_release();
    if (err) { set_state(UPD_ERROR, err); return; }
    pmutex_lock(&s_lock);
    snprintf(s_st.version, sizeof s_st.version, "%s", s_rel.version);
    pmutex_unlock(&s_lock);
    u8 cur[32];
    bool same = file_sha(s_nro, cur, NULL) && !memcmp(cur, s_rel.sha, 32);
    int vc = version_cmp(s_rel.version, MKDD_VERSION);
    rt_log("update: release %s (%llu bytes, %s), running %s from %s: %s", s_rel.version, s_rel.size, s_rel.url,
           MKDD_VERSION, s_nro,
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
    const char *err = fetch(s_rel.url, &s);
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
    /* sockets only while the updater works (unless LAN play has them): they hold transfer memory */
    if (!plat_net_up()) {
        rt_log("update: no sockets");
        set_state(UPD_ERROR, "NETWORK UNAVAILABLE");
    } else {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        if (install) do_install();
        else do_check();
        curl_global_cleanup();
        plat_net_down();
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
