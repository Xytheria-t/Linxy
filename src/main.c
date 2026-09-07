/*
 * Linxy - hotkey launcher/toggler for Windows.
 * One slot per letter A..Z, configured in shortcuts.ini next to the exe.
 * Default combos: Shift+Alt+<letter> toggles (launch/show/hide),
 * Shift+Alt+Ctrl+<letter> terminates the target app.
 * .lnk targets keep their arguments, so browser-installed apps (PWAs) launch
 * and toggle as the app itself instead of opening a blank browser.
 * Build with mingw-w64 (see Makefile). Single zero-dependency exe.
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <objbase.h>
#include <propsys.h>
#include <wchar.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEYS  26                /* A..Z */
#define PATH_CAP  4096
#define INI_NAME  L"shortcuts.ini"

typedef struct {
    WCHAR path[PATH_CAP];   /* target executable (resolved from a .lnk) */
    WCHAR args[PATH_CAP];   /* arguments kept from the .lnk, empty for plain paths */
    WCHAR appname[128];     /* .lnk file base name (= web app name) if any */
    WCHAR appid[128];       /* --app-id hash from the .lnk args, if any */
    int   app;              /* 1 = browser-installed app (PWA): toggle its windows */
    HWND  snap[16];         /* windows visible at hide time, restored on show */
    int   nsnap;
    HWND  last_main;        /* real main window last seen visible; survives the
                               snapshot going stale (tray-resident apps) */
    DWORD last_launch;      /* tick of the last launch: re-presses while the
                               browser is still creating the window must not
                               start a second one (PWA relaunch never focuses
                               the pending window, it opens another) */
    int   used;
} Slot;

static Slot slots[MAX_KEYS];

/* ---- config ------------------------------------------------------------ */

static void config_path(WCHAR *out, size_t cap)
{
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    for (WCHAR *p = out + n; p > out; p--) {
        if (*p == L'\\') { *p = L'\0'; break; }
    }
    wcsncat(out, L"\\" INI_NAME, cap - wcslen(out) - 1);
}

static void strip_ws(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = '\0';
}

/* Returns slot index 0..25 for a single letter, or -1. */
static int parse_key(const char *k)
{
    if (k[1] != '\0') return -1;
    if (k[0] >= 'a' && k[0] <= 'z') return k[0] - 'a';
    if (k[0] >= 'A' && k[0] <= 'Z') return k[0] - 'A';
    return -1;
}

static void to_wide(const char *utf8, WCHAR *out, int cap)
{
    out[0] = L'\0';   /* stays empty if both conversions fail */
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, cap))
        return;
    /* Not UTF-8 (e.g. ANSI config) - fall back to the system codepage. */
    MultiByteToWideChar(CP_ACP, 0, utf8, -1, out, cap);
}

/* Bounded wide-string append; always NUL-terminated (_snwprintf alone leaves
   the buffer unterminated when it truncates). */
static void warn_append(WCHAR *dst, size_t cap, const WCHAR *fmt, ...)
{
    size_t used = wcslen(dst);
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(dst + used, cap - used - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - used - 1)
        dst[cap - 1] = L'\0';
}

/* Bounded wide-string copy; dst is always NUL-terminated. */
static void copy_wide(WCHAR *dst, int cap, const WCHAR *src)
{
    size_t n = wcslen(src);
    if (n >= (size_t)cap) n = (size_t)cap - 1;
    memcpy(dst, src, n * sizeof(WCHAR));
    dst[n] = L'\0';
}

/* Resolve a .lnk shortcut to its executable path and arguments.
   Returns 1 on success; args is set to L"" (possibly with arguments). */
static int resolve_lnk(const WCHAR *lnk, WCHAR *out, int cap, WCHAR *args, int argcap)
{
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    int ok = 0;
    if (args) args[0] = L'\0';
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IShellLinkW, (void **)&sl))) {
        if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
            if (SUCCEEDED(pf->lpVtbl->Load(pf, lnk, STGM_READ))) {
                WIN32_FIND_DATAW fd;
                if (SUCCEEDED(sl->lpVtbl->GetPath(sl, out, cap, &fd, SLGP_UNCPRIORITY)))
                    ok = out[0] != L'\0';
                if (ok && args)
                    sl->lpVtbl->GetArguments(sl, args, argcap);
            }
            pf->lpVtbl->Release(pf);
        }
        sl->lpVtbl->Release(sl);
    }
    return ok;
}

static int app_marker(const WCHAR *args, WCHAR *out, int cap);   /* defined below */
static const WCHAR *wcs_isearch(const WCHAR *hay, const WCHAR *needle);   /* defined below */

static void load_config(void)
{
    WCHAR path[PATH_CAP + 32];
    config_path(path, PATH_CAP + 32);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        MessageBoxW(NULL, L"未找到 shortcuts.ini,请把它放在 Linxy.exe 同目录。",
                    L"Linxy", MB_ICONINFORMATION);
        return;
    }
    DWORD sz = GetFileSize(f, NULL);
    if (sz == INVALID_FILE_SIZE || sz > 65536) sz = 65536;
    char *buf = malloc(sz + 1);
    if (!buf) { CloseHandle(f); return; }
    if (!ReadFile(f, buf, sz, &sz, NULL)) sz = 0;
    CloseHandle(f);
    buf[sz] = '\0';
    if ((unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, sz - 2);   /* strip UTF-8 BOM */
        buf[sz - 3] = '\0';
    }
    if (!*buf) {
        MessageBoxW(NULL, L"shortcuts.ini 是空的,没有注册任何热键。",
                    L"Linxy", MB_ICONINFORMATION);
        free(buf);
        return;
    }

    WCHAR warn[1024] = L"";
    int nwarn = 0, line_no = 0;
#define WARN(...) do { \
        if (nwarn < 4) { \
            warn_append(warn, sizeof warn / sizeof warn[0], __VA_ARGS__); \
            nwarn++; \
        } \
    } while (0)
    for (char *line = buf, *next; *line; line = next) {
        line_no++;
        next = strchr(line, '\n');
        if (next) { *next = '\0'; next++; }
        else next = line + strlen(line);
        strip_ws(line);
        if (!*line || *line == ';') continue;

        char *eq = strchr(line, '=');
        if (!eq) {
            WARN(L"第 %d 行:缺少\"=\"\n", line_no);
            continue;
        }
        *eq = '\0';
        char *key = line, *val = eq + 1;
        strip_ws(val);

        /* Unknown directives are reported; slot lines are handled below. */
        if (strlen(key) != 1) {
            WCHAR wkey[32];
            to_wide(key, wkey, 32);
            WARN(L"第 %d 行:未知指令 \"%s\"\n", line_no, wkey);
            continue;
        }
        int idx = parse_key(key);
        if (idx < 0 || !*val) {
            WARN(L"第 %d 行:键应为单个字母 A~Z,且要有目标路径\n", line_no);
            continue;
        }
        WCHAR path[PATH_CAP];
        to_wide(val, path, PATH_CAP);
        size_t plen = wcslen(path);
        if (plen >= 4 && _wcsicmp(path + plen - 4, L".lnk") == 0) {
            if (!resolve_lnk(path, slots[idx].path, PATH_CAP, slots[idx].args, PATH_CAP)) {
                WARN(L"第 %d 行:快捷方式无法解析\n", line_no);
                continue;
            }
            /* The .lnk file name is the installed web app's name (e.g.
               DeepSeek.lnk -> "DeepSeek"). Used to pick the app's own windows
               by title. */
            const WCHAR *b = wcsrchr(path, L'\\');
            b = b ? b + 1 : path;
            copy_wide(slots[idx].appname,
                      sizeof slots[idx].appname / sizeof slots[idx].appname[0], b);
            WCHAR *dot = wcsrchr(slots[idx].appname, L'.');
            if (dot) *dot = L'\0';
            {
                WCHAR marker[PATH_CAP];
                slots[idx].app = app_marker(slots[idx].args, marker, PATH_CAP);
            }
            /* Chromium stamps every PWA window with an AppUserModelID carrying
               the --app-id hash, so the app's windows can be found without
               relying on the title (which varies with page and locale). */
            const WCHAR *idp = wcs_isearch(slots[idx].args, L"--app-id=");
            if (idp) {
                idp += 9;
                int t = 0;
                while (idp[t] && idp[t] != L' ' && idp[t] != L'"' &&
                       t < (int)(sizeof slots[idx].appid / sizeof slots[idx].appid[0]) - 1)
                    t++;
                memcpy(slots[idx].appid, idp, t * sizeof(WCHAR));
                slots[idx].appid[t] = L'\0';
            }
        } else {
            /* Plain path, optionally quoted with launch arguments after it:
               X="C:\dir with spaces\app.exe" -flag1 -flag2 */
            const WCHAR *cq = (path[0] == L'"') ? wcschr(path + 1, L'"') : NULL;
            if (cq) {
                size_t pl = (size_t)(cq - path - 1);
                if (pl >= PATH_CAP) pl = PATH_CAP - 1;
                memcpy(slots[idx].path, path + 1, pl * sizeof(WCHAR));
                slots[idx].path[pl] = L'\0';
                const WCHAR *a = cq + 1;
                while (*a == L' ' || *a == L'\t') a++;
                copy_wide(slots[idx].args,
                          sizeof slots[idx].args / sizeof slots[idx].args[0], a);
            } else {
                copy_wide(slots[idx].path,
                          sizeof slots[idx].path / sizeof slots[idx].path[0], path);
                slots[idx].args[0] = L'\0';
            }
            slots[idx].appname[0] = L'\0';
            slots[idx].app = 0;
        }
        slots[idx].used = 1;
    }
#undef WARN
    free(buf);
    if (nwarn)
        MessageBoxW(NULL, warn, L"Linxy - 配置警告", MB_ICONWARNING);
}

/* ---- process lookup ----------------------------------------------------- */

static const WCHAR *base_name(const WCHAR *path)
{
    const WCHAR *b = path;
    for (const WCHAR *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') b = p + 1;
    return b;
}

/* base != NULL: match by executable name. base == NULL: match full image path. */
static int match_pass(const WCHAR *full, const WCHAR *base, DWORD *out, int cap)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    int n = 0;
    if (Process32FirstW(snap, &pe)) do {
        int hit = 0;
        if (base) {
            hit = _wcsicmp(pe.szExeFile, base) == 0;
        } else {
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (p) {
                WCHAR img[PATH_CAP];
                DWORD sz = PATH_CAP;
                hit = QueryFullProcessImageNameW(p, 0, img, &sz) && _wcsicmp(img, full) == 0;
                CloseHandle(p);
            }
        }
        if (hit && n < cap) out[n++] = pe.th32ProcessID;
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return n;
}

/* ---- case-insensitive substring helpers ---------------------------------- */

static WCHAR wch_lower(WCHAR c)
{
    return (c >= L'A' && c <= L'Z') ? c - L'A' + L'a' : c;
}

static const WCHAR *wcs_isearch(const WCHAR *hay, const WCHAR *needle)
{
    size_t nl = wcslen(needle);
    if (!nl) return hay;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] && wch_lower(hay[i]) == wch_lower(needle[i])) i++;
        if (i == nl) return hay;
    }
    return NULL;
}

/*
 * If the .lnk arguments carry a PWA-style "--app..." token (--app=URL or
 * --app-id=<hash>), copy that token into out and return 1. That token alone
 * identifies the app's own process, telling it apart from the plain browser.
 */
static int app_marker(const WCHAR *args, WCHAR *out, int cap)
{
    const WCHAR *p = args;
    while (*p) {
        while (*p == L' ') p++;
        if (!*p) break;
        int t = 0;
        WCHAR tok[PATH_CAP];
        if (*p == L'"') {
            p++;
            while (*p && *p != L'"' && t < PATH_CAP - 1) tok[t++] = *p++;
            if (*p == L'"') p++;
        } else {
            while (*p && *p != L' ' && t < PATH_CAP - 1) tok[t++] = *p++;
        }
        tok[t] = L'\0';
        if (wcsncmp(tok, L"--app", 5) == 0 &&
            (tok[5] == L'=' || tok[5] == L'-' || tok[5] == L'\0')) {
            copy_wide(out, cap, tok);
            return 1;
        }
    }
    return 0;
}

/*
 * Find PIDs of the target app, incl. manual starts. Exact image path first;
 * aliases (e.g. Windows 11 notepad launches from WindowsApps) never match the
 * configured path, so fall back to the base name on a second pass.
 */
static int find_pids(const WCHAR *image, DWORD *out, int cap)
{
    int n = match_pass(image, NULL, out, cap);
    if (n == 0) n = match_pass(NULL, base_name(image), out, cap);
    return n;
}

typedef struct {
    HWND *wins;
    int   n, cap;
    const DWORD *pids;
    int   npids;
} EnumCtx;

static BOOL CALLBACK enum_proc(HWND h, LPARAM lp)
{
    EnumCtx *c = (EnumCtx *)lp;
    DWORD pid;
    GetWindowThreadProcessId(h, &pid);
    for (int i = 0; i < c->npids; i++) {
        if (c->pids[i] == pid) {
            if (c->n < c->cap) c->wins[c->n++] = h;
            break;
        }
    }
    return TRUE;
}

static int collect_windows(const DWORD *pids, int npids, HWND *out, int cap)
{
    EnumCtx c = { out, 0, cap, pids, npids };
    EnumWindows(enum_proc, (LPARAM)&c);
    return c.n;
}

/* ---- web app (PWA) windows ----------------------------------------------
   A browser-installed app such as DeepSeek is just a window inside the shared
   browser process, so processes cannot tell it apart from normal browser tabs.
   Chromium tags each app window's AppUserModelID with the --app-id hash of its
   shortcut; slots launched via --app=<url> carry no hash and fall back to
   picking windows by title + owning process directory. */

/* 0 = window has no AppUserModelID, 1 = AUMID present but not ours,
   2 = AUMID carries our app's --app-id hash. */
static int win_appid(HWND h, const WCHAR *appid)
{
    static const IID kIID_IPropertyStore =
        {0x886D8EEB,0x8CF2,0x4446,{0x8F,0x02,0x9F,0xBB,0x0F,0xEF,0xFE,0x40}};
    static const PROPERTYKEY kPKEY_AppUserModel_ID =
        {{0x9F4C2855,0x9F79,0x4B39,{0xA8,0xD0,0xE1,0xD4,0x2D,0xE1,0xD5,0xF3}},5};
    IPropertyStore *ps;
    int r = 0;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(h, &kIID_IPropertyStore, (void **)&ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->lpVtbl->GetValue(ps, &kPKEY_AppUserModel_ID, &v)) &&
            v.vt == VT_LPWSTR && v.pwszVal)
            r = wcs_isearch(v.pwszVal, appid) ? 2 : 1;
        PropVariantClear(&v);
        ps->lpVtbl->Release(ps);
    }
    return r;
}

/* True if window h belongs to a process whose image lives under dir. */
static int win_in_dir(HWND h, const WCHAR *dir)
{
    DWORD pid;
    GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return 0;
    WCHAR img[PATH_CAP];
    DWORD sz = PATH_CAP;
    int ok = QueryFullProcessImageNameW(p, 0, img, &sz);
    CloseHandle(p);
    if (!ok) return 0;
    size_t dl = wcslen(dir);
    if (_wcsnicmp(img, dir, dl) != 0) return 0;
    return img[dl] == L'\0' || img[dl] == L'\\';
}

typedef struct {
    HWND        *wins;
    int          n, cap;
    const WCHAR *dir;
    const WCHAR *name;
    const WCHAR *appid;
} PwaCtx;

static BOOL CALLBACK pwa_enum(HWND h, LPARAM lp)
{
    PwaCtx *c = (PwaCtx *)lp;
    if (c->n >= c->cap) return TRUE;
    if (*c->appid) {
        int a = win_appid(h, c->appid);
        if (a == 2) { c->wins[c->n++] = h; return TRUE; }
        /* AUMID present but different: not this app. Plain browser windows
           always carry the browser's own id, so a browser tab whose page
           title merely contains the app name is never captured. Only windows
           with no AUMID at all (non-Chromium hosts) use the title heuristic. */
        if (a == 1) return TRUE;
    }
    if (win_in_dir(h, c->dir)) {
        WCHAR t[256];
        if (GetWindowTextW(h, t, sizeof t / sizeof t[0]) > 0 &&
            wcs_isearch(t, c->name))
            c->wins[c->n++] = h;
    }
    return TRUE;
}

static int collect_pwa_windows(const Slot *s, HWND *out, int cap)
{
    WCHAR dir[PATH_CAP];
    wcscpy(dir, s->path);
    WCHAR *sl = wcsrchr(dir, L'\\');
    if (sl) *sl = L'\0';
    PwaCtx c = { out, 0, cap, dir, s->appname, s->appid };
    EnumWindows(pwa_enum, (LPARAM)&c);
    return c.n;
}

/* One AttachThreadInput foreground grab. Returns 1 if hwnd ended foreground. */
static int grab_foreground(HWND hwnd, DWORD cur, DWORD tgt)
{
    HWND fg = GetForegroundWindow();
    DWORD fgt = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    if (fgt && fgt != cur && fgt != tgt) AttachThreadInput(cur, fgt, TRUE);
    if (cur != tgt) AttachThreadInput(cur, tgt, TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    if (cur != tgt) AttachThreadInput(cur, tgt, FALSE);
    if (fgt && fgt != cur && fgt != tgt) AttachThreadInput(cur, fgt, FALSE);
    return GetForegroundWindow() == hwnd;
}

static void focus(HWND hwnd)
{
    /* This process is usually not the foreground owner (we are awoken by a
       hotkey), so SetForegroundWindow below is often blocked by the foreground
       lock. A topmost flash brings the window in front reliably, then drops it
       back to a normal z-order. Retry: the foreground lock can lift a moment
       after the flash, and tray-resident apps stuff a window they never
       actually became foreground back into the tray. */
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    DWORD cur = GetCurrentThreadId(), tgt = GetWindowThreadProcessId(hwnd, NULL);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (grab_foreground(hwnd, cur, tgt)) return;
        Sleep(80);
    }
    /* The foreground lock can still refuse us (the user is typing in another
       app, an elevated window holds focus). Chromium-based targets then stay
       in their native-occlusion "hidden" state: the window shows up but never
       repaints and swallows every click. A minimize + restore fixes both -
       SW_RESTORE out of the minimized state carries activation rights that a
       background process's SetForegroundWindow lacks, and the transition
       forces the occlusion tracker to re-evaluate the window. */
    ShowWindow(hwnd, SW_MINIMIZE);
    Sleep(200);
    ShowWindow(hwnd, SW_RESTORE);
    if (GetForegroundWindow() == hwnd) return;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (grab_foreground(hwnd, cur, tgt)) return;
        Sleep(80);
    }
}

/* ---- actions ------------------------------------------------------------ */

static void launch_slot(const Slot *s)
{
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    WCHAR cmdline[PATH_CAP * 2 + 8];
    if (*s->args)
        _snwprintf(cmdline, PATH_CAP * 2 + 8, L"\"%s\" %s", s->path, s->args);
    else
        _snwprintf(cmdline, PATH_CAP * 2 + 8, L"\"%s\"", s->path);
    if (!CreateProcessW(s->path, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        DWORD err = GetLastError();
        /* ERROR_ELEVATION_REQUIRED = the target needs admin rights and we are
           not elevated. Retry through the shell with "runas" so Windows raises
           a UAC consent prompt instead of just failing (error 740). */
        if (err == ERROR_ELEVATION_REQUIRED) {
            HINSTANCE hr = ShellExecuteW(NULL, L"runas", s->path,
                                         *s->args ? s->args : NULL, NULL, SW_SHOWNORMAL);
            if ((INT_PTR)hr > 32)   /* any value > 32 means success */
                return;             /* pi is uninitialized here, do not touch it */
            err = (DWORD)(INT_PTR)hr;
        }
        WCHAR msg[PATH_CAP + 80];
        _snwprintf(msg, PATH_CAP + 80, L"无法启动目标应用:\n%s\n(错误码 %lu,请确认 shortcuts.ini 里是绝对路径)",
                   s->path, err);
        MessageBoxW(NULL, msg, L"Linxy", MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

/* True if the window looks like a real, usable main window (has a title, is
   sizable, is not a tray/toolbox helper). Auxiliary stubs that pop up as a
   blank window are rejected so we relaunch instead of showing them. */
static int looks_main(HWND h)
{
    WCHAR t[128];
    if (GetWindowTextW(h, t, sizeof t / sizeof t[0]) == 0) return 0;
    RECT r;
    if (!GetWindowRect(h, &r) || r.right - r.left < 80 || r.bottom - r.top < 40) return 0;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return 0;
    return 1;
}

/* ---- toggle building blocks (shared by normal and web-app slots) --------- */

static int any_unminimized(const HWND *wins, int n)
{
    for (int i = 0; i < n; i++)
        if (IsWindowVisible(wins[i]) && !IsIconic(wins[i])) return 1;
    return 0;
}

/* True if a real main window is on screen (visible and un-minimized).
   Satellite windows (desktop lyrics, toast popups, tool windows) never
   count, so they cannot shadow a tray-hidden main window. */
static int any_visible_main(const HWND *wins, int n)
{
    for (int i = 0; i < n; i++)
        if (IsWindowVisible(wins[i]) && !IsIconic(wins[i]) && looks_main(wins[i]))
            return 1;
    return 0;
}

/* Snapshot the currently visible windows, then hide exactly those. */
static void hide_snapshot(Slot *s, const HWND *wins, int n)
{
    s->nsnap = 0;
    for (int i = 0; i < n && s->nsnap < 16; i++)
        if (IsWindowVisible(wins[i]))
            s->snap[s->nsnap++] = wins[i];
    for (int i = 0; i < s->nsnap; i++) ShowWindow(s->snap[i], SW_HIDE);
}

/* Show again what we hid, focusing the first. Returns how many survived -
   0 means the whole snapshot went stale (windows destroyed externally). */
static int revive_snapshot(Slot *s)
{
    int revived = 0;
    for (int i = 0; i < s->nsnap; i++) {
        if (!IsWindow(s->snap[i])) continue;
        if (IsIconic(s->snap[i]))
            ShowWindow(s->snap[i], SW_RESTORE);
        else
            ShowWindow(s->snap[i], SW_SHOW);
        if (!revived) focus(s->snap[i]);
        revived = 1;
    }
    s->nsnap = 0;
    return revived;
}

/* Bring back a window we never hid ourselves: a manually minimized one first,
   else an already-visible main window. Returns 1 if one was focused. */
static int revive_any(const HWND *wins, int n)
{
    for (int i = 0; i < n; i++)
        if (IsWindowVisible(wins[i]) && IsIconic(wins[i])) {
            ShowWindow(wins[i], SW_RESTORE);
            focus(wins[i]);
            return 1;
        }
    for (int i = 0; i < n; i++)
        if (IsWindowVisible(wins[i]) && !IsIconic(wins[i]) && looks_main(wins[i])) {
            focus(wins[i]);
            return 1;
        }
    return 0;
}

/*
 * Track the app's real main window: the largest *visible* main-looking window.
 * Tray-resident apps keep this window alive but hidden while minimized to
 * tray, and spawn a second process if relaunched - so bring it back instead of
 * launching. Stubs (blank Chrome_WidgetWin_1) are never visible, so they never
 * win this fight.
 */
static void note_main(Slot *s, const HWND *wins, int n)
{
    long best = 0;
    for (int i = 0; i < n; i++) {
        HWND h = wins[i];
        if (!IsWindowVisible(h) || IsIconic(h)) continue;
        if (!looks_main(h)) continue;
        RECT r;
        if (!GetWindowRect(h, &r)) continue;
        long a = (long)(r.right - r.left) * (long)(r.bottom - r.top);
        if (a > best) { best = a; s->last_main = h; }
    }
}

/* Restore a hidden-but-alive main window so a second hotkey press reopens the
   app instead of starting a duplicate process. Prefer last_main (a window we
   actually saw visible); otherwise pick the largest hidden main-looking window
   of the current processes - the real window, since blank stubs are smaller.
   Returns 1 if one was shown and focused. */
static int revive_hidden_main(Slot *s, const HWND *wins, int n)
{
    HWND pick = NULL;
    if (s->last_main)
        for (int i = 0; i < n; i++)
            if (wins[i] == s->last_main) { pick = s->last_main; break; }
    long best = 0;
    for (int i = 0; i < n; i++) {
        HWND h = wins[i];
        if (IsWindowVisible(h) || IsIconic(h)) continue;
        if (!looks_main(h)) continue;
        RECT r;
        if (!GetWindowRect(h, &r)) continue;
        long w = r.right - r.left, hh = r.bottom - r.top;
        if (w < 400 || hh < 300) continue;   /* too small to be the real window */
        long a = w * hh;
        if (a > best) { best = a; if (!pick) pick = h; }
    }
    if (!pick) return 0;
    ShowWindow(pick, IsIconic(pick) ? SW_RESTORE : SW_SHOW);
    focus(pick);
    return 1;
}

/* Web-app slots toggle at the window level: hide / restore / reopen the
   app's own window(s) regardless of whether the browser runs them as a
   separate process or hosts them inside the shared browser instance. */
static void pwa_toggle(Slot *s)
{
    HWND wins[64];
    int n = collect_pwa_windows(s, wins, 64);
    note_main(s, wins, n);

    if (any_unminimized(wins, n)) {
        hide_snapshot(s, wins, n);
        return;
    }
    if (revive_snapshot(s)) return;
    if (revive_any(wins, n)) return;
    if (revive_hidden_main(s, wins, n)) return;
    /* Chromium needs a few seconds to put the launched window on screen, and
       relaunching the same --app-id opens another window instead of focusing
       the pending one. Ignore presses inside that gap. */
    DWORD now = GetTickCount64();
    if (s->last_launch && now - s->last_launch < 4000) return;
    s->last_launch = now;
    launch_slot(s);   /* snapshot fully stale and nothing to revive */
}

/* Closing a web app means closing its window(s), not killing the shared
   browser process the window may live in. */
static void pwa_kill(Slot *s)
{
    s->nsnap = 0;
    s->last_main = NULL;
    HWND wins[64];
    int n = collect_pwa_windows(s, wins, 64);
    for (int i = 0; i < n; i++)
        if (IsWindow(wins[i])) PostMessageW(wins[i], WM_CLOSE, 0, 0);
}

static void toggle_slot(Slot *s)
{
    if (s->app) { pwa_toggle(s); return; }
    DWORD pids[16];
    int npids = find_pids(s->path, pids, 16);

    HWND wins[128];
    int n = 0;
    if (npids > 0) n = collect_windows(pids, npids, wins, 128);
    note_main(s, wins, n);   /* remember the real main window before hiding it */

    /* A tray-resident app closed with Alt+F4/× keeps its real main window
       alive but hidden, while a satellite window (e.g. NetEase desktop
       lyrics) can still be on screen. Bring the main window back instead of
       hiding the satellite again - otherwise the toggle keeps shuffling the
       lyrics window and the main one never returns. */
    if (!any_visible_main(wins, n)) {
        int revived = (s->nsnap) ? revive_snapshot(s) : 0;
        if (revive_hidden_main(s, wins, n)) revived = 1;   /* main gets focus last */
        if (revived) return;
    }

    /* Any un-minimized window: remember exactly what is visible, hide it. */
    for (int i = 0; i < n; i++) {
        if (IsWindowVisible(wins[i]) && !IsIconic(wins[i])) {
            hide_snapshot(s, wins, n);
            return;
        }
    }
    /* Restore what we hid. If every window in the snapshot was destroyed
       externally while the process stayed resident (tray-resident apps),
       fall back to the hidden real main window before relaunching - launch
       only when it is really gone. */
    if (s->nsnap) {
        if (revive_snapshot(s)) return;
        if (revive_hidden_main(s, wins, n)) return;
        launch_slot(s);
        return;
    }
    /* Nothing hidden by us and nothing un-minimized: a manually minimized
       window or an already-visible main window can be brought forward.
       A tray-resident process (minimized to tray on its own, e.g. after
       being Alt+F4'd) keeps its real window alive but hidden - show that
       instead of starting a duplicate instance. Only launch when it is
       genuinely gone (or single-instance forwarding is the only way). */
    if (!revive_any(wins, n) &&
        !revive_hidden_main(s, wins, n))
        launch_slot(s);
}

static void kill_slot(Slot *s)
{
    if (s->app) { pwa_kill(s); return; }
    s->nsnap = 0;
    s->last_main = NULL;
    DWORD pids[16];
    int npids = find_pids(s->path, pids, 16);
    if (npids == 0) return;

    /* First ask nicely: WM_CLOSE on the app's top-level windows behaves like
       Alt+F4, so apps quit cleanly (saving state, removing their tray icon).
       A tray-resident app keeps its real window hidden, so the remembered
       main window gets the request too. Only processes that ignore the
       request or have no window get hard-terminated below. */
    HWND wins[128];
    int n = collect_windows(pids, npids, wins, 128);
    for (int i = 0; i < n; i++)
        if (IsWindowVisible(wins[i]) || IsIconic(wins[i]) || wins[i] == s->last_main)
            PostMessageW(wins[i], WM_CLOSE, 0, 0);

    /* Give the app a moment to exit on its own before terminating. */
    for (int round = 0; round < 30; round++) {
        Sleep(100);
        int alive = 0;
        for (int i = 0; i < npids && !alive; i++) {
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                   FALSE, pids[i]);
            if (p) {
                DWORD code = 0;
                if (GetExitCodeProcess(p, &code) && code == STILL_ACTIVE) alive = 1;
                CloseHandle(p);
            }
        }
        if (!alive) return;
    }

    for (int i = 0; i < npids; i++) {
        if (pids[i] == GetCurrentProcessId()) continue;
        HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pids[i]);
        if (p) { TerminateProcess(p, 1); CloseHandle(p); }
    }
}

static void append_combo(WCHAR *dst, size_t cap, UINT mod, WCHAR ch)
{
    WCHAR tmp[64];
    size_t w = 0;
#define PUSH(s) do { const WCHAR *_s = (s); while (*_s && w < 62) tmp[w++] = *_s++; } while (0)
    if (mod & MOD_CONTROL) { PUSH(L"Ctrl");  tmp[w++] = L'+'; }
    if (mod & MOD_ALT)     { PUSH(L"Alt");   tmp[w++] = L'+'; }
    if (mod & MOD_SHIFT)   { PUSH(L"Shift"); tmp[w++] = L'+'; }
#undef PUSH
    tmp[w++] = ch;
    tmp[w] = L'\0';
    size_t used = wcslen(dst);
    int n = _snwprintf(dst + used, cap - used - 1, L"%s\n", tmp);
    if (n < 0 || (size_t)n >= cap - used - 1) dst[cap - 1] = L'\0';
}

/* ---- main --------------------------------------------------------------- */

/* Slot action request: bit 0 = kill, bits 1.. = slot index. */
static HANDLE g_work_sem = NULL;

typedef struct { int slot; int kill; } WorkItem;

#define WORKQ_CAP 64
static WorkItem workq[WORKQ_CAP];
static int workq_head, workq_count;
static CRITICAL_SECTION workq_cs;

static DWORD WINAPI worker_thread(LPVOID arg)
{
    (void)arg;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* ShellExecuteW in launch_slot */
    for (;;) {
        WaitForSingleObject(g_work_sem, INFINITE);
        EnterCriticalSection(&workq_cs);
        if (!workq_count) { LeaveCriticalSection(&workq_cs); continue; }
        WorkItem it = workq[workq_head];
        workq_head = (workq_head + 1) % WORKQ_CAP;
        workq_count--;
        LeaveCriticalSection(&workq_cs);

        Slot *s = &slots[it.slot];
        if (it.kill) kill_slot(s);
        else         toggle_slot(s);
    }
    return 0;
}

/* Queue the action and return at once: toggle/kill can block on a busy target
   (ShowWindow / SendMessage to another thread), and the hotkey loop must never
   stall while that happens. */
static void post_action(int idx, int kill)
{
    EnterCriticalSection(&workq_cs);
    if (workq_count < WORKQ_CAP) {
        workq[(workq_head + workq_count) % WORKQ_CAP] = (WorkItem){ idx, kill };
        workq_count++;
        LeaveCriticalSection(&workq_cs);
        ReleaseSemaphore(g_work_sem, 1, NULL);
    } else {
        LeaveCriticalSection(&workq_cs);   /* queue full: drop, hotkey loop stays alive */
    }
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;

    if (CreateMutexW(NULL, TRUE, L"Local\\Linxy") &&
        GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;   /* already running (e.g. twice at boot): exit quietly */

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    load_config();

    WCHAR taken[1024] = L"以下组合已被其他程序占用,已跳过:\n";
    int ntaken = 0;
    /* Register every configured slot; ntaken only caps how many conflicts
       get listed in the report, never how many slots get registered. */
    for (int i = 0; i < MAX_KEYS; i++) {
        if (!slots[i].used) continue;
        UINT vk = 'A' + i;
        UINT ids[2] = { i * 2, i * 2 + 1 };
        UINT mods[2] = { MOD_ALT | MOD_SHIFT, MOD_ALT | MOD_SHIFT | MOD_CONTROL };
        for (int k = 0; k < 2; k++) {
            if (RegisterHotKey(NULL, ids[k], mods[k], vk)) continue;
            if (ntaken < 8) {
                append_combo(taken, sizeof taken / sizeof taken[0], mods[k], (WCHAR)vk);
                ntaken++;
            }
        }
    }
    if (ntaken)
        MessageBoxW(NULL, taken, L"Linxy - 热键冲突", MB_ICONWARNING);

    InitializeCriticalSection(&workq_cs);
    g_work_sem = CreateSemaphoreW(NULL, 0, WORKQ_CAP, NULL);
    CloseHandle(CreateThread(NULL, 0, worker_thread, NULL, 0, NULL));

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY) {
            int idx = (int)msg.wParam / 2;
            if (idx >= 0 && idx < MAX_KEYS && slots[idx].used)
                post_action(idx, (int)(msg.wParam & 1));
        } else {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}