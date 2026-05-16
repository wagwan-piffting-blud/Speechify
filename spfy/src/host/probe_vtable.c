/*
 * probe_vtable.c — Windows runtime probe for SWIttsFe-en-US.dll's 49-slot vtable.
 *
 * Builds as a 32-bit console exe with MSVC or MinGW32:
 *     cl /MD /O2 probe_vtable.c
 *     gcc -m32 -O2 -o probe_vtable.exe probe_vtable.c
 *
 * Run from the directory containing SWIttsFe-en-US.dll (and its data files
 * if any are loaded lazily, though static analysis says enu.ddl is baked in).
 * Emits probe_vtable.csv with one row per slot.
 *
 * Strategy for each slot:
 *   - All wrappers take `this` as param_1, plus 0-3 user args.
 *   - For pure setters (slots 19/22/25/39-46), we pass canary values and
 *     re-read via Release-time state inspection (skipped in this probe).
 *   - For getters/predicates we pass zeros and capture the return value.
 *   - For methods that look like they invoke deep FE pipeline (slots 3,4,8),
 *     we time them and capture the return.
 *   - For DictionarySet (27-38), we just call with NULLs and let them
 *     error out — the return code is informative on its own.
 *
 * NB: This intentionally calls everything with default 0 args. Slots that
 * crash will be caught by SEH and reported as "FAULT". For deeper signal
 * the user should run a follow-up probe after we identify how to wire
 * delegate-A (the text source).
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>

typedef int (__stdcall *getObject_fn)(int kind, void **out);

/* Vtable function pointer typedefs — we only need a generic catch-all here. */
typedef uint32_t (__stdcall *vfn0)(void *this_);
typedef uint32_t (__stdcall *vfn1)(void *this_, uint32_t a);
typedef uint32_t (__stdcall *vfn2)(void *this_, uint32_t a, uint32_t b);
typedef uint32_t (__stdcall *vfn3)(void *this_, uint32_t a, uint32_t b, uint32_t c);

/* Slot N → number of user args (from vtable_inventory.md). 0 means just `this`. */
static const int g_slot_user_args[49] = {
    /* 0  */ 2,  /* QueryInterface(kind, out)              */
    /* 1  */ 0,  /* AddRef                                 */
    /* 2  */ 0,  /* Release                                */
    /* 3  */ 0,  /* initStage1                             */
    /* 4  */ 0,  /* initStage2                             */
    /* 5  */ 1,  /* feedConfigA(s)                         */
    /* 6  */ 1,  /* feedConfigB(s)                         */
    /* 7  */ 2,  /* logEvent(a,b)                          */
    /* 8  */ 0,  /* synth                                  */
    /* 9  */ 3,  /* delegateA_call(a,b,c)                  */
    /* 10 */ 3,  /* getErrorMessage(buf, n, _)             */
    /* 11 */ 1,  /* runOrAbort(b)                          */
    /* 12 */ 0,  /* notifyEvent                            */
    /* 13 */ 0,  /* cancel                                 */
    /* 14 */ 0,  /* isReady                                */
    /* 15 */ 1,  /* predicateA(x)                          */
    /* 16 */ 1,  /* predicateB(x)                          */
    /* 17 */ 2,  /* predicateC(x,y)                        */
    /* 18 */ 2,  /* setterA(x,y)                           */
    /* 19 */ 2,  /* setPair_A(a,b)                         */
    /* 20 */ 2,  /* setterB(x,y)                           */
    /* 21 */ 2,  /* setterC(x,y)                           */
    /* 22 */ 2,  /* setPair_D(a,b)                         */
    /* 23 */ 1,  /* predicateD(x)                          */
    /* 24 */ 2,  /* predicateD_full(x,y)                   */
    /* 25 */ 1,  /* setMode(b)                             */
    /* 26 */ 0,  /* reset                                  */
    /* 27 */ 1,  /* DictionarySet_load                     */
    /* 28 */ 1,  /* DictionarySet_fileLoad                 */
    /* 29 */ 1,  /* DictionarySet_free                     */
    /* 30 */ 1,  /* DictionarySet_activate                 */
    /* 31 */ 1,  /* DictionarySet_deactByName              */
    /* 32 */ 1,  /* DictionarySet_deactivate               */
    /* 33 */ 1,  /* getKind                                */
    /* 34 */ 1,  /* DictionarySet_update                   */
    /* 35 */ 1,  /* DictionarySet_findFirst                */
    /* 36 */ 1,  /* DictionarySet_findNext                 */
    /* 37 */ 1,  /* DictionarySet_lookup                   */
    /* 38 */ 1,  /* DictionarySet_prioLookup               */
    /* 39 */ 2,  /* setPair_B(a,b)                         */
    /* 40 */ 2,  /* setPair_C(a,b)                         */
    /* 41 */ 2,  /* setPair_E(a,b)                         */
    /* 42 */ 3,  /* delegateB_call(a,b,c)                  */
    /* 43 */ 2,  /* setPair_F(a,b)                         */
    /* 44 */ 3,  /* delegateB_call2(a,b,c)                 */
    /* 45 */ 2,  /* setPair_G(a,b)                         */
    /* 46 */ 2,  /* setPair_H(a,b)                         */
    /* 47 */ 3,  /* installHookA                           */
    /* 48 */ 3,  /* installHookB                           */
};

static const char *g_slot_name[49] = {
    "QueryInterface","AddRef","Release","initStage1","initStage2",
    "feedConfigA","feedConfigB","logEvent","synth","delegateA_call",
    "getErrorMessage","runOrAbort","notifyEvent","cancel","isReady",
    "predicateA","predicateB","predicateC","setterA","setPair_A",
    "setterB","setterC","setPair_D","predicateD","predicateD_full",
    "setMode","reset","DictionarySet_load","DictionarySet_fileLoad",
    "DictionarySet_free","DictionarySet_activate","DictionarySet_deactByName",
    "DictionarySet_deactivate","getKind","DictionarySet_update",
    "DictionarySet_findFirst","DictionarySet_findNext","DictionarySet_lookup",
    "DictionarySet_prioLookup","setPair_B","setPair_C","setPair_E",
    "delegateB_call","setPair_F","delegateB_call2","setPair_G","setPair_H",
    "installHookA","installHookB"
};

static uint32_t call_slot(void *obj, int slot, uint32_t a, uint32_t b, uint32_t c) {
    void ***vt = (void ***)obj;
    void *fn = (*vt)[slot];
    int n = g_slot_user_args[slot];
    /* MSVC-style SEH (__try/__except) catches access violations when a
     * slot fault-touches caller-owned memory. mingw GCC doesn't support
     * those keywords, so on this toolchain the probe will simply abort
     * the process on a bad slot — run with MSVC if you want soft
     * recovery. */
#ifdef _MSC_VER
    __try {
        switch (n) {
            case 0: return ((vfn0)fn)(obj);
            case 1: return ((vfn1)fn)(obj, a);
            case 2: return ((vfn2)fn)(obj, a, b);
            case 3: return ((vfn3)fn)(obj, a, b, c);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xDEADBEEF;
    }
#else
    switch (n) {
        case 0: return ((vfn0)fn)(obj);
        case 1: return ((vfn1)fn)(obj, a);
        case 2: return ((vfn2)fn)(obj, a, b);
        case 3: return ((vfn3)fn)(obj, a, b, c);
    }
#endif
    return 0xBADBAD;
}

int main(int argc, char **argv) {
    const char *dll_name = argc > 1 ? argv[1] : "SWIttsFe-en-US.dll";

    HMODULE h = LoadLibraryA(dll_name);
    if (!h) {
        fprintf(stderr, "LoadLibrary(%s) failed: 0x%lx\n",
                dll_name, GetLastError());
        return 1;
    }

    getObject_fn getObject = (getObject_fn)GetProcAddress(h, "getObject");
    if (!getObject) {
        fprintf(stderr, "getObject not exported\n");
        return 2;
    }

    void *obj = NULL;
    int rc = getObject(1, &obj);
    if (!rc || !obj) {
        fprintf(stderr, "getObject(1, ...) returned %d, obj=%p\n", rc, obj);
        return 3;
    }
    fprintf(stderr, "obj=%p (refcount=%u after getObject)\n",
            obj, *(uint32_t *)((uint8_t *)obj + 4));

    FILE *out = fopen("probe_vtable.csv", "w");
    if (!out) { perror("fopen"); return 4; }
    fprintf(out, "slot,name,user_args,return_hex,return_dec,note\n");

    /* Skip slot 2 (Release) and slot 0 (QueryInterface) — we'll do those at
     * the end so the object stays alive for slots 3..48. */
    for (int slot = 3; slot < 49; slot++) {
        /* Slot 8 (synth) is heavy. Probe it last so we have the rest of the
         * vector before any state change. */
        if (slot == 8) continue;
        DWORD t0 = GetTickCount();
        uint32_t r = call_slot(obj, slot, 0, 0, 0);
        DWORD dt = GetTickCount() - t0;
        const char *note = "";
        if (r == 0xDEADBEEF) note = "FAULT";
        else if (dt > 50) note = "SLOW";
        fprintf(out, "%d,%s,%d,0x%08x,%d,%s%s%dms\n",
                slot, g_slot_name[slot], g_slot_user_args[slot],
                r, (int32_t)r, note, (*note ? " " : ""), (int)dt);
    }
    /* Now slot 8. */
    {
        DWORD t0 = GetTickCount();
        uint32_t r = call_slot(obj, 8, 0, 0, 0);
        DWORD dt = GetTickCount() - t0;
        const char *note = (r == 0xDEADBEEF) ? "FAULT" : "";
        fprintf(out, "%d,%s,%d,0x%08x,%d,%s%s%dms\n",
                8, g_slot_name[8], g_slot_user_args[8],
                r, (int32_t)r, note, (*note ? " " : ""), (int)dt);
    }

    /* QueryInterface — kind=1, kind=2, kind=3 to confirm allowed values. */
    {
        void *qi_out = NULL;
        uint32_t r1 = ((vfn2)((*(void ***)obj))[0])(obj, 1, (uint32_t)&qi_out);
        fprintf(out, "0,%s_kind1,2,0x%08x,%d,out=%p\n",
                g_slot_name[0], r1, (int32_t)r1, qi_out);
        if (qi_out) ((vfn0)((*(void ***)qi_out))[2])(qi_out);
        qi_out = NULL;
        uint32_t r2 = ((vfn2)((*(void ***)obj))[0])(obj, 2, (uint32_t)&qi_out);
        fprintf(out, "0,%s_kind2,2,0x%08x,%d,out=%p\n",
                g_slot_name[0], r2, (int32_t)r2, qi_out);
        if (qi_out) ((vfn0)((*(void ***)qi_out))[2])(qi_out);
        qi_out = NULL;
        uint32_t r3 = ((vfn2)((*(void ***)obj))[0])(obj, 3, (uint32_t)&qi_out);
        fprintf(out, "0,%s_kind3,2,0x%08x,%d,out=%p (expected NULL)\n",
                g_slot_name[0], r3, (int32_t)r3, qi_out);
    }

    /* AddRef / Release at the end. */
    uint32_t refc = ((vfn0)((*(void ***)obj))[1])(obj);
    fprintf(out, "1,AddRef,0,0x%08x,%u,refcount after AddRef\n", refc, refc);
    refc = ((vfn0)((*(void ***)obj))[2])(obj);
    fprintf(out, "2,Release,0,0x%08x,%u,refcount after Release\n", refc, refc);
    refc = ((vfn0)((*(void ***)obj))[2])(obj);
    fprintf(out, "2,Release_final,0,0x%08x,%u,destruct\n", refc, refc);

    fclose(out);
    FreeLibrary(h);
    fprintf(stderr, "wrote probe_vtable.csv\n");
    return 0;
}
