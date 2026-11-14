/* The Windows updater (switch/update.h; the launcher's Updates page and the in-game UPDATE item use it). What the
 * latest release is comes from GitHub, api.github.com/repos/<repo>/releases/latest (no update.txt): its tag and
 * either a MKDD-NX.exe asset or the MKDD-NX-Windows-<version>.zip, with the size and SHA-256 ("digest") GitHub
 * records on upload. The download is checked against both, a zip is unpacked with Windows' own tar.exe (Windows 10
 * 1803 and later), then the running .exe is renamed to MKDD-NX.exe.old (Windows lets a running .exe be renamed,
 * not overwritten) and the new one takes its name; the .old is deleted at the next start. An update is offered
 * when the release is newer, or the same version with a different .exe (a MKDD-NX.exe asset only: a zip's .exe
 * can't be compared before it's downloaded). No token: the repository has to be public while players update. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "../runtime/rt.h"
#include "../switch/update.h"
#include "../switch/update_json.h"
#include "mkdd_version.h"

#define REPO "digdat0/mkdd-nx"
#define API_PATH L"/repos/" REPO "/releases/latest"
#define ASSET_URL "https://github.com/" REPO "/releases/download/"
#define API_MAX (512 << 10)

static CRITICAL_SECTION s_lock;
static UpdateStatus s_st;
static bool s_busy;
static wchar_t s_exe[MAX_PATH], s_dir[MAX_PATH];
static struct { char version[24]; unsigned long long size; uint8_t sha[32]; char url[384]; bool zip; } s_rel;

const char *update_version(void) { return MKDD_VERSION; }

void update_init(const char *nro_path) {
    (void)nro_path;
    InitializeCriticalSection(&s_lock);
    DWORD n = GetModuleFileNameW(NULL, s_exe, MAX_PATH);
    if (!n || n >= MAX_PATH) { s_exe[0] = 0; return; }
    wcscpy(s_dir, s_exe);
    wchar_t *sl = wcsrchr(s_dir, L'\\');
    if (sl) *sl = 0;
    wchar_t old[MAX_PATH + 8];
    swprintf(old, MAX_PATH + 8, L"%ls.old", s_exe);
    DeleteFileW(old); /* the one an update replaced (still in use if that copy hasn't quite exited: next time) */
}

void update_status(UpdateStatus *out) {
    EnterCriticalSection(&s_lock);
    *out = s_st;
    LeaveCriticalSection(&s_lock);
}

static void set_state(int state, const char *msg) {
    EnterCriticalSection(&s_lock);
    s_st.state = state;
    if (msg) snprintf(s_st.msg, sizeof s_st.msg, "%s", msg);
    LeaveCriticalSection(&s_lock);
    if (msg) rt_log("update: %s", msg);
}

static void set_progress(float p) {
    EnterCriticalSection(&s_lock);
    s_st.progress = p;
    LeaveCriticalSection(&s_lock);
}

/* ---- SHA-256 (CNG) ---- */
typedef struct { BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h; } Sha;
static bool sha_open(Sha *s) {
    memset(s, 0, sizeof *s);
    if (BCryptOpenAlgorithmProvider(&s->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return false;
    if (BCryptCreateHash(s->alg, &s->h, NULL, 0, NULL, 0, 0) < 0) { BCryptCloseAlgorithmProvider(s->alg, 0); return false; }
    return true;
}
static void sha_add(Sha *s, const void *p, size_t n) { BCryptHashData(s->h, (PUCHAR)p, (ULONG)n, 0); }
static void sha_done(Sha *s, uint8_t out[32]) {
    BCryptFinishHash(s->h, out, 32, 0);
    BCryptDestroyHash(s->h);
    BCryptCloseAlgorithmProvider(s->alg, 0);
}
static bool file_sha(const wchar_t *path, uint8_t out[32], unsigned long long *size) {
    FILE *f = _wfopen(path, L"rb");
    Sha s;
    if (!f) return false;
    if (!sha_open(&s)) { fclose(f); return false; }
    static uint8_t buf[256 << 10];
    unsigned long long n = 0;
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) sha_add(&s, buf, r), n += r;
    fclose(f);
    sha_done(&s, out);
    if (size) *size = n;
    return true;
}

/* ---- HTTP (WinHTTP: follows the release download's redirect to GitHub's CDN by itself) ---- */
/* GET url into buf (cap bytes, NUL-terminated) or into the file f (hashed on the way, progress against total).
 * Returns NULL or what went wrong (menu font letters). */
static const char *fetch(const char *url, char *buf, size_t cap, FILE *f, Sha *sha, unsigned long long total) {
    wchar_t wurl[512], host[128], path[400];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 512)) return "BAD DOWNLOAD ADDRESS";
    URL_COMPONENTSW uc = {sizeof uc};
    uc.lpszHostName = host, uc.dwHostNameLength = 128, uc.lpszUrlPath = path, uc.dwUrlPathLength = 400;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return "BAD DOWNLOAD ADDRESS";
    const char *err = NULL;
    DWORD code = 0, gle = 0;
    HINTERNET ses = WinHttpOpen(L"MKDD-NX/" MKDD_VERSION " (Windows)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"MKDD-NX/" MKDD_VERSION " (Windows)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0); /* before Windows 8.1 */
    HINTERNET con = ses ? WinHttpConnect(ses, host, uc.nPort, 0) : NULL;
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (req) WinHttpSetTimeouts(req, 20000, 20000, 30000, 30000);
    const wchar_t *hdr = wcsstr(host, L"api.github.com") ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n"
                                                         : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (!req || !WinHttpSendRequest(req, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req, NULL)) {
        gle = GetLastError();
        err = gle == ERROR_WINHTTP_NAME_NOT_RESOLVED || gle == ERROR_WINHTTP_CANNOT_CONNECT ? "NO INTERNET CONNECTION"
              : gle == ERROR_WINHTTP_TIMEOUT ? "THE CONNECTION TIMED OUT: TRY AGAIN" : "DOWNLOAD FAILED: TRY AGAIN";
        goto done;
    }
    DWORD sz = sizeof code;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
    if (code == 404) { err = "NO RELEASE FOUND (IS THE REPOSITORY PUBLIC?)"; goto done; }
    if (code == 403 || code == 429) { err = "GITHUB IS BUSY: TRY AGAIN IN AN HOUR"; goto done; }
    if (code != 200) { err = "THE SERVER REFUSED: TRY AGAIN LATER"; goto done; }
    size_t len = 0;
    unsigned long long got = 0;
    static char chunk[64 << 10];
    for (;;) {
        DWORD rd = 0;
        if (!WinHttpReadData(req, chunk, sizeof chunk, &rd)) {
            gle = GetLastError();
            err = gle == ERROR_WINHTTP_TIMEOUT ? "THE CONNECTION TIMED OUT: TRY AGAIN" : "DOWNLOAD FAILED: TRY AGAIN";
            break;
        }
        if (!rd) break;
        if (f) {
            if (fwrite(chunk, 1, rd, f) != rd) { err = "COULD NOT WRITE TO THE GAME FOLDER"; break; }
            sha_add(sha, chunk, rd);
            got += rd;
            if (total) set_progress((float)((double)got / (double)total));
            if (total && got > total) { err = "THE DOWNLOAD IS DAMAGED: TRY AGAIN"; break; }
        } else {
            if (len + rd >= cap) { err = "THE RELEASE INFO IS TOO BIG"; break; }
            memcpy(buf + len, chunk, rd);
            len += rd;
            buf[len] = 0;
        }
    }
done:
    rt_log("update: GET %s -> HTTP %lu, error %lu: %s", url, (unsigned long)code, (unsigned long)gle, err ? err : "ok");
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return err;
}

/* releases/latest: the tag and the Windows asset's size, digest and url into s_rel (a bare MKDD-NX.exe first) */
static bool parse_release(const char *t) {
    memset(&s_rel, 0, sizeof s_rel);
    char tag[32], name[96], digest[80], zipname[96];
    if (!str_val(member(t, "tag_name"), tag, sizeof tag)) return false;
    const char *v = tag + (tag[0] == 'v' || tag[0] == 'V');
    if (!isdigit((unsigned char)*v)) return false;
    snprintf(s_rel.version, sizeof s_rel.version, "%s", v);
    snprintf(zipname, sizeof zipname, "MKDD-NX-Windows-%s.zip", s_rel.version);
    const char *a = member(t, "assets"), *pick = NULL;
    if (!a || *a != '[') return false;
    for (a = ws(a + 1); *a == '{'; a = ws(a + 1)) {
        if (!skip(a)) return false; /* cut off inside it: a number could be short */
        if (str_val(member(a, "name"), name, sizeof name)) {
            if (!strcmp(name, "MKDD-NX.exe")) { pick = a, s_rel.zip = false; break; }
            if (!strcmp(name, zipname) && !pick) pick = a, s_rel.zip = true;
        }
        if (!(a = skip(a)) || *(a = ws(a)) != ',') break;
    }
    if (!pick) return false;
    const char *size = member(pick, "size");
    s_rel.size = size && isdigit((unsigned char)*size) ? strtoull(size, NULL, 10) : 0;
    return str_val(member(pick, "digest"), digest, sizeof digest) && !strncmp(digest, "sha256:", 7) && strlen(digest) == 7 + 64 &&
           parse_sha(digest + 7, s_rel.sha) && str_val(member(pick, "browser_download_url"), s_rel.url, sizeof s_rel.url) &&
           !strncmp(s_rel.url, ASSET_URL, strlen(ASSET_URL)) && s_rel.size > 0x1000 && s_rel.size < (512ull << 20);
}

static void do_check(void) {
    char *buf = calloc(1, API_MAX);
    if (!buf) { set_state(UPD_ERROR, "OUT OF MEMORY"); return; }
    const char *err = fetch("https://api.github.com" "/repos/" REPO "/releases/latest", buf, API_MAX, NULL, NULL, 0);
    if (!err && !parse_release(buf)) err = "THE RELEASE HAS NO WINDOWS DOWNLOAD";
    free(buf);
    if (err) { set_state(UPD_ERROR, err); return; }
    EnterCriticalSection(&s_lock);
    snprintf(s_st.version, sizeof s_st.version, "%s", s_rel.version);
    LeaveCriticalSection(&s_lock);
    uint8_t cur[32];
    int vc = version_cmp(s_rel.version, MKDD_VERSION);
    bool same = vc == 0 && (s_rel.zip || (file_sha(s_exe, cur, NULL) && !memcmp(cur, s_rel.sha, 32)));
    rt_log("update: release %s (%s, %llu bytes), running %s: %s", s_rel.version, s_rel.url, s_rel.size, MKDD_VERSION,
           same ? "the same" : vc < 0 ? "older" : "newer or rebuilt");
    set_state(same || vc < 0 ? UPD_CURRENT : UPD_AVAILABLE, NULL);
}

/* tar.exe -xf zip -C dir MKDD-NX/MKDD-NX.exe, no window; true when it ran and exited 0 */
static bool unzip_exe(const wchar_t *zip, const wchar_t *dir) {
    wchar_t tar[MAX_PATH], cmd[3 * MAX_PATH + 64];
    UINT n = GetSystemDirectoryW(tar, MAX_PATH - 16);
    if (!n || n >= MAX_PATH - 16) return false;
    wcscat(tar, L"\\tar.exe");
    swprintf(cmd, sizeof cmd / sizeof *cmd, L"\"%ls\" -xf \"%ls\" -C \"%ls\" MKDD-NX/MKDD-NX.exe", tar, zip, dir);
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(tar, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        rt_log("update: tar.exe didn't start (%lu)", GetLastError());
        return false;
    }
    DWORD rc = 1;
    if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &rc);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    rt_log("update: tar.exe exit %lu", rc);
    return rc == 0;
}

static bool is_exe(const wchar_t *path, unsigned long long *size) {
    FILE *f = _wfopen(path, L"rb");
    char mz[2] = {0};
    bool ok = f && fread(mz, 1, 2, f) == 2 && mz[0] == 'M' && mz[1] == 'Z';
    if (f) { _fseeki64(f, 0, SEEK_END); *size = (unsigned long long)_ftelli64(f); fclose(f); }
    return ok && *size > (1u << 20);
}

static void do_install(void) {
    wchar_t dl[MAX_PATH + 32], tmpdir[MAX_PATH + 32], neu[MAX_PATH + 64], old[MAX_PATH + 8];
    swprintf(dl, MAX_PATH + 32, L"%ls\\MKDD-NX.update.download", s_dir);
    swprintf(tmpdir, MAX_PATH + 32, L"%ls\\MKDD-NX.update", s_dir);
    swprintf(old, MAX_PATH + 8, L"%ls.old", s_exe);
    DeleteFileW(dl);
    FILE *f = _wfopen(dl, L"wb");
    if (!f) { set_state(UPD_ERROR, "COULD NOT WRITE TO THE GAME FOLDER"); return; }
    setvbuf(f, NULL, _IOFBF, 256 << 10);
    Sha sha;
    if (!sha_open(&sha)) { fclose(f); DeleteFileW(dl); set_state(UPD_ERROR, "COULD NOT START THE DOWNLOAD"); return; }
    const char *err = fetch(s_rel.url, NULL, 0, f, &sha, s_rel.size);
    if (fclose(f) != 0 && !err) err = "COULD NOT WRITE TO THE GAME FOLDER";
    uint8_t got[32], disk[32];
    sha_done(&sha, got);
    unsigned long long size = 0;
    /* what landed on the disk, not only what came over the wire */
    if (!err && (!file_sha(dl, disk, &size) || size != s_rel.size || memcmp(disk, s_rel.sha, 32) || memcmp(got, s_rel.sha, 32)))
        err = "THE DOWNLOAD IS DAMAGED: TRY AGAIN";
    if (!err && s_rel.zip) {
        CreateDirectoryW(tmpdir, NULL);
        swprintf(neu, MAX_PATH + 64, L"%ls\\MKDD-NX\\MKDD-NX.exe", tmpdir);
        DeleteFileW(neu);
        if (!unzip_exe(dl, tmpdir)) err = "COULD NOT UNPACK THE DOWNLOAD";
    } else if (!err) {
        swprintf(neu, MAX_PATH + 64, L"%ls", dl);
    }
    if (!err && !is_exe(neu, &size)) err = "THE DOWNLOAD IS NOT THE GAME";
    if (!err) {
        /* swap: running -> .old (allowed while it runs), new -> running; back again if the second step fails */
        DeleteFileW(old);
        if (!MoveFileExW(s_exe, old, MOVEFILE_REPLACE_EXISTING)) err = "COULD NOT REPLACE THE GAME (IS THE FOLDER READ-ONLY?)";
        else if (!MoveFileExW(neu, s_exe, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            MoveFileExW(old, s_exe, MOVEFILE_REPLACE_EXISTING);
            err = "COULD NOT REPLACE THE GAME (IS THE FOLDER READ-ONLY?)";
        }
    }
    DeleteFileW(dl);
    if (s_rel.zip) {
        swprintf(neu, MAX_PATH + 64, L"%ls\\MKDD-NX\\MKDD-NX.exe", tmpdir);
        DeleteFileW(neu);
        swprintf(neu, MAX_PATH + 64, L"%ls\\MKDD-NX", tmpdir);
        RemoveDirectoryW(neu), RemoveDirectoryW(tmpdir);
    }
    if (err) { set_state(UPD_ERROR, err); return; }
    rt_log("update: installed %s (the previous .exe kept as MKDD-NX.exe.old until the next start)", s_rel.version);
    set_progress(1);
    set_state(UPD_READY, NULL);
}

static DWORD WINAPI worker(void *arg) {
    if (arg) do_install();
    else do_check();
    EnterCriticalSection(&s_lock);
    s_busy = false;
    LeaveCriticalSection(&s_lock);
    return 0;
}

void update_start(void) {
    if (!s_exe[0]) { set_state(UPD_ERROR, "CAN'T FIND THE GAME'S .EXE"); return; }
    EnterCriticalSection(&s_lock);
    if (s_busy || s_st.state == UPD_READY) { LeaveCriticalSection(&s_lock); return; }
    bool install = s_st.state == UPD_AVAILABLE;
    s_busy = true;
    s_st.state = install ? UPD_DOWNLOADING : UPD_CHECKING;
    s_st.progress = 0;
    s_st.msg[0] = 0;
    LeaveCriticalSection(&s_lock);
    HANDLE h = CreateThread(NULL, 0, worker, install ? (void *)1 : NULL, 0, NULL);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); return; }
    EnterCriticalSection(&s_lock);
    s_busy = false;
    LeaveCriticalSection(&s_lock);
    set_state(UPD_ERROR, "COULD NOT START THE UPDATER");
}
