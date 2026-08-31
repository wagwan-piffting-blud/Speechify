#include "voice_find.h"

#include "env.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define MAX_ROOTS 24

/* ---------------------------------------------------------------- paths -- */

#ifdef _WIN32
#define SEP '\\'
#define IS_SEP(c) ((c) == '\\' || (c) == '/')
#else
#define SEP '/'
#define IS_SEP(c) ((c) == '/')
#endif

static int ci_eq(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static int ci_cmp(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y) return x < y ? -1 : 1;
    }
    return (*a ? 1 : 0) - (*b ? 1 : 0);
}

static void join(char *dst, size_t n, const char *a, const char *b)
{
    snprintf(dst, n, "%s%c%s", a, SEP, b);
}

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int dir_exists(const char *p)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* A BCP-47-shaped directory name: two letters, '-', two-or-more letters or
 * digits. Recognising the SHAPE is what lets a new language directory work
 * without touching this file. */
static int looks_like_lang(const char *s)
{
    size_t i;
    if (!isalpha((unsigned char)s[0]) || !isalpha((unsigned char)s[1])) return 0;
    if (s[2] != '-') return 0;
    for (i = 3; s[i]; ++i)
        if (!isalnum((unsigned char)s[i])) return 0;
    return i >= 5;
}

/* ------------------------------------------------------------ dir walks -- */

typedef void (*dir_cb)(const char *name, void *ctx);

/* Call `cb` for every SUBDIRECTORY of `dir`, "." and ".." excluded. */
static void for_each_subdir(const char *dir, dir_cb cb, void *ctx)
{
#ifdef _WIN32
    char pat[1024];
    snprintf(pat, sizeof pat, "%s%c*", dir, SEP);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        cb(fd.cFileName, ctx);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char full[1024];
        if (e->d_name[0] == '.') continue;
        join(full, sizeof full, dir, e->d_name);
        if (!dir_exists(full)) continue;
        cb(e->d_name, ctx);
    }
    closedir(d);
#endif
}

/* --------------------------------------------------------------- roots --- */

typedef struct {
    char v[MAX_ROOTS][1024];
    size_t n;
} roots_t;

typedef struct { int found; } has_lang_ctx;

static void has_lang_cb(const char *name, void *ctx)
{
    if (looks_like_lang(name)) ((has_lang_ctx *)ctx)->found = 1;
}

/* A root is a directory that CONTAINS at least one language directory. That
 * test is what makes "run it from anywhere in the tree" work without a
 * hard-coded repo path. */
static int is_root(const char *dir)
{
    has_lang_ctx c = {0};
    if (!dir_exists(dir)) return 0;
    for_each_subdir(dir, has_lang_cb, &c);
    return c.found;
}

static void push_root(roots_t *r, const char *dir)
{
    size_t i;
    if (!dir || !*dir || r->n >= MAX_ROOTS) return;
    for (i = 0; i < r->n; ++i)
        if (ci_eq(r->v[i], dir)) return;
    snprintf(r->v[r->n], sizeof r->v[0], "%s", dir);
    r->n++;
}

/* `dir` and up to `up` of its ancestors, each added if it is a root. */
static void push_root_chain(roots_t *r, const char *dir, int up)
{
    char cur[1024];
    snprintf(cur, sizeof cur, "%s", dir);
    for (;;) {
        size_t n = strlen(cur);
        while (n > 1 && IS_SEP(cur[n - 1])) cur[--n] = '\0';
        if (n == 0) return;
        if (is_root(cur)) push_root(r, cur);
        if (up-- <= 0) return;
        while (n > 0 && !IS_SEP(cur[n - 1])) --n;
        if (n <= 1) return;              /* "/" or "C:\" -- stop */
        cur[n - 1] = '\0';
    }
}

static char g_search_path[MAX_ROOTS * 1024];

#ifdef _WIN32
/* The user's REAL Documents folder, which is not reliably
 * %USERPROFILE%\Documents: OneDrive redirects it, and Windows records the
 * redirection in the shell folder table.
 *
 * ⚠ spfy_sapi.dll has always resolved the voice root with
 * SHGetFolderPath(CSIDL_PERSONAL), so on a redirected machine the SAPI voice
 * loads happily from a directory this file could not see. Anything comparing
 * "what is installed" against a manifest -- spfy_update -- would then find no
 * voices at all and report everything fine. --list-voices was wrong there too,
 * just less consequentially.
 *
 * shell32 is loaded on demand rather than linked: spfy_common is in every
 * target, including the WASM and unix builds, and none of them should grow an
 * import for this. */
static int win_documents_dir(char *buf, size_t buf_n)
{
    typedef HRESULT (WINAPI *pfn_shgfp)(HWND, int, HANDLE, DWORD, LPSTR);
    HMODULE h;
    pfn_shgfp fn;
    char path[MAX_PATH];
    int ok = 0;

    if (buf_n < MAX_PATH) return 0;
    h = LoadLibraryA("shell32.dll");
    if (!h) return 0;
    {
        FARPROC p = GetProcAddress(h, "SHGetFolderPathA");
        if (p) {
            /* memcpy, not *(FARPROC *)&fn: writing through a pointer of a
             * different function type breaks strict aliasing and gcc warns at
             * -Wstrict-aliasing in the x64 build. memcpy is the portable way
             * to convert a GetProcAddress result and compiles to the same
             * register move. */
            memcpy(&fn, &p, sizeof fn);
            /* 0x0005 = CSIDL_PERSONAL, 0 = SHGFP_TYPE_CURRENT: the path in
             * effect now, not the default one. */
            if (fn(NULL, 0x0005, NULL, 0, path) == S_OK && path[0]) {
                snprintf(buf, buf_n, "%s", path);
                ok = 1;
            }
        }
    }
    FreeLibrary(h);
    return ok;
}
#endif

/* Search order, most specific first:
 *   1. SPFY_VOICE_DIR         -- explicit override, no guessing
 *   2. the working directory and its ancestors   (running inside the tree)
 *   3. the executable's directory and its ancestors  (an installed copy)
 *   4. the shell's Documents\Speechify (Windows; follows a OneDrive
 *      redirection, which is what spfy_sapi.dll resolves)
 *   5. ~/Documents/Speechify  -- the installer's layout
 * Each candidate has to LOOK like a root (hold a language directory) before
 * it counts, so a stray ancestor contributes nothing. */
static void collect_roots(const char *argv0, roots_t *r)
{
    char buf[1024];
    const char *e;

    r->n = 0;

    e = spfy_env("SPFY_VOICE_DIR");
    if (e && *e) {
        push_root_chain(r, e, 0);
        /* Honour it even when it holds voice folders directly rather than
         * language folders -- see resolve_in_root(). */
        if (r->n == 0 && dir_exists(e)) push_root(r, e);
    }

#ifdef _WIN32
    if (GetCurrentDirectoryA((DWORD)sizeof buf, buf) > 0)
        push_root_chain(r, buf, 6);
#else
    if (getcwd(buf, sizeof buf)) push_root_chain(r, buf, 6);
#endif

    if (argv0 && *argv0) {
        size_t n = strlen(argv0);
        while (n > 0 && !IS_SEP(argv0[n - 1])) --n;
        if (n > 1 && n < sizeof buf) {
            memcpy(buf, argv0, n - 1);
            buf[n - 1] = '\0';
            push_root_chain(r, buf, 6);
        }
    }

#ifdef _WIN32
    /* Before %USERPROFILE%: on a redirected profile this is the one the SAPI
     * DLL actually loads from, and push_root de-duplicates when they agree. */
    {
        char docs[MAX_PATH];
        if (win_documents_dir(docs, sizeof docs)) {
            snprintf(buf, sizeof buf, "%s%cSpeechify", docs, SEP);
            if (is_root(buf)) push_root(r, buf);
        }
    }
#endif

    e = spfy_env(
#ifdef _WIN32
        "USERPROFILE"
#else
        "HOME"
#endif
    );
    if (e && *e) {
        snprintf(buf, sizeof buf, "%s%cDocuments%cSpeechify", e, SEP, SEP);
        if (is_root(buf)) push_root(r, buf);
    }

    {
        size_t i, o = 0;
        g_search_path[0] = '\0';
        for (i = 0; i < r->n && o + 2 < sizeof g_search_path; ++i)
            o += (size_t)snprintf(g_search_path + o, sizeof g_search_path - o,
                                  "%s%s", i ? "\n" : "", r->v[i]);
    }
}

const char *spfy_voice_search_path(void)
{
    return g_search_path;
}

/* --------------------------------------------------------- the triple --- */

/* Fill the three paths for a voice folder. Returns 0 when all three exist. */
static int fill_triple(const char *lang, const char *dir, const char *real,
                       spfy_voice_paths *out)
{
    char stem[1200];
    int ok = 1;

    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", real);
    snprintf(out->lang, sizeof out->lang, "%s", lang ? lang : "");
    snprintf(out->dir, sizeof out->dir, "%s", dir);

    snprintf(stem, sizeof stem, "%s%c%s", dir, SEP, real);

    snprintf(out->vin, sizeof out->vin, "%s.vin", stem);
    if (!file_exists(out->vin)) { out->vin[0] = '\0'; ok = 0; }

    snprintf(out->vcf, sizeof out->vcf, "%s.vcf", stem);
    if (!file_exists(out->vcf)) { out->vcf[0] = '\0'; ok = 0; }

    /* ⛔ `<name>8.vdb` FIRST AND ALWAYS. A voice can ship a 16 kHz bank
     * beside the 8 kHz one, and picking it silently changes what the engine
     * concatenates -- see the "never use the 16k vdb" note. Only when there
     * is no 8 kHz bank at all does the plain name apply. */
    snprintf(out->vdb, sizeof out->vdb, "%s8.vdb", stem);
    if (!file_exists(out->vdb)) {
        snprintf(out->vdb, sizeof out->vdb, "%s.vdb", stem);
        if (!file_exists(out->vdb)) { out->vdb[0] = '\0'; ok = 0; }
    }

    return ok ? 0 : -2;
}

/* ------------------------------------------------------------- resolve --- */

typedef struct {
    const char      *want;      /* requested name, any casing */
    const char      *lang;
    const char      *langdir;
    spfy_voice_paths *out;
    int              hit;       /* 0 none, 1 complete, -2 incomplete */
} find_ctx;

static void find_voice_cb(const char *name, void *vctx)
{
    find_ctx *c = (find_ctx *)vctx;
    char dir[1024];
    if (c->hit == 1) return;
    if (!ci_eq(name, c->want)) return;
    join(dir, sizeof dir, c->langdir, name);
    c->hit = fill_triple(c->lang, dir, name, c->out) == 0 ? 1 : -2;
}

typedef struct {
    find_ctx *fc;
    const char *root;
} lang_ctx;

static void find_lang_cb(const char *name, void *vctx)
{
    lang_ctx *lc = (lang_ctx *)vctx;
    char langdir[1024];
    if (lc->fc->hit == 1) return;
    if (!looks_like_lang(name)) return;
    join(langdir, sizeof langdir, lc->root, name);
    lc->fc->lang = name;
    lc->fc->langdir = langdir;
    for_each_subdir(langdir, find_voice_cb, lc->fc);
}

static int resolve_in_root(const char *root, const char *want,
                           spfy_voice_paths *out)
{
    find_ctx fc = {0};
    lang_ctx lc;
    char direct[1024];

    fc.want = want;
    fc.out = out;
    lc.fc = &fc;
    lc.root = root;
    for_each_subdir(root, find_lang_cb, &lc);
    if (fc.hit) return fc.hit == 1 ? 0 : -2;

    /* A root given by SPFY_VOICE_DIR may hold voice folders directly. */
    join(direct, sizeof direct, root, want);
    if (dir_exists(direct)) {
        find_ctx d = {0};
        d.want = want; d.out = out; d.lang = ""; d.langdir = root;
        for_each_subdir(root, find_voice_cb, &d);
        if (d.hit) return d.hit == 1 ? 0 : -2;
    }
    return -1;
}

int spfy_voice_is_name(const char *s)
{
    size_t i, n;
    if (!s || !*s) return 0;
    n = strlen(s);
    for (i = 0; i < n; ++i)
        if (IS_SEP(s[i]) || s[i] == ':') return 0;
    if (n > 4) {
        const char *e = s + n - 4;
        if (ci_eq(e, ".vin") || ci_eq(e, ".vdb") || ci_eq(e, ".vcf")
            || ci_eq(e, ".wav") || ci_eq(e, ".txt")) return 0;
    }
    return 1;
}

int spfy_voice_resolve(const char *name, const char *argv0,
                       spfy_voice_paths *out)
{
    roots_t roots;
    size_t i;
    int worst = -1;

    if (!name || !*name || !out) return -1;

    /* A path to the voice DIRECTORY works too: `spfy_synth en-US/crstom ...`
     * is what a shell's tab-completion produces, and refusing it would be
     * pedantry. The folder's own name is the stem. */
    if (!spfy_voice_is_name(name) && dir_exists(name)) {
        char dir[1024];
        size_t n;
        const char *base;
        snprintf(dir, sizeof dir, "%s", name);
        n = strlen(dir);
        while (n > 1 && IS_SEP(dir[n - 1])) dir[--n] = '\0';
        base = dir + n;
        while (base > dir && !IS_SEP(base[-1])) --base;
        return fill_triple("", dir, base, out);
    }

    collect_roots(argv0, &roots);
    for (i = 0; i < roots.n; ++i) {
        int rc = resolve_in_root(roots.v[i], name, out);
        if (rc == 0) return 0;
        if (rc == -2) worst = -2;
    }
    return worst;
}

/* ---------------------------------------------------------------- list --- */

typedef struct {
    spfy_voice_paths *out;
    size_t            n, max;
    const char       *lang;
    const char       *langdir;
} list_ctx;

static void list_voice_cb(const char *name, void *vctx)
{
    list_ctx *c = (list_ctx *)vctx;
    spfy_voice_paths p;
    char dir[1024];
    size_t i;

    join(dir, sizeof dir, c->langdir, name);
    if (fill_triple(c->lang, dir, name, &p) != 0) return;
    for (i = 0; i < c->n; ++i)
        if (ci_eq(c->out[i].name, p.name) && ci_eq(c->out[i].lang, p.lang))
            return;                     /* first root wins */
    if (c->n >= c->max) return;
    c->out[c->n++] = p;
}

typedef struct { list_ctx *lc; const char *root; } list_lang_ctx;

static void list_lang_cb(const char *name, void *vctx)
{
    list_lang_ctx *l = (list_lang_ctx *)vctx;
    char langdir[1024];
    if (!looks_like_lang(name)) return;
    join(langdir, sizeof langdir, l->root, name);
    l->lc->lang = name;
    l->lc->langdir = langdir;
    for_each_subdir(langdir, list_voice_cb, l->lc);
}

static int cmp_voice(const void *a, const void *b)
{
    const spfy_voice_paths *x = (const spfy_voice_paths *)a;
    const spfy_voice_paths *y = (const spfy_voice_paths *)b;
    int c = ci_cmp(x->lang, y->lang);
    return c ? c : ci_cmp(x->name, y->name);
}

size_t spfy_voice_list(const char *argv0, spfy_voice_paths *out, size_t max)
{
    roots_t roots;
    list_ctx lc = {0};
    size_t i;

    if (!out || max == 0) return 0;
    lc.out = out;
    lc.max = max;

    collect_roots(argv0, &roots);
    for (i = 0; i < roots.n; ++i) {
        list_lang_ctx l;
        l.lc = &lc;
        l.root = roots.v[i];
        for_each_subdir(roots.v[i], list_lang_cb, &l);
    }
    if (lc.n > 1) qsort(out, lc.n, sizeof *out, cmp_voice);
    return lc.n;
}
