#include <windows.h>

typedef int (*Create2Fn)(void* (*alloc)(int), int, void**);
typedef int (*Destroy2Fn)(void*);
typedef int (*CountFn)(void*, int*);
typedef int (*PmLogFn)(void*, int, void*);
typedef int (*Create1Fn)(void* (*alloc)(int), int);
typedef int (*Destroy1Fn)(void);
typedef int (*Od5TempFn)(int, int, void*);
typedef int (*Od5ActFn)(int, void*);

static void mark(int* fault, int value) {
    if (fault) *fault = value;
}

int seh_adl_create2(void* fn, void* (*alloc)(int), int flags, void** ctx, int* fault) {
    if (!fn || !alloc || !ctx) {
        mark(fault, 1);
        return -1;
    }
    mark(fault, 0);
    __try {
        return ((Create2Fn)fn)(alloc, flags, ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mark(fault, 1);
        return -1;
    }
}

int seh_adl_destroy2(void* fn, void* ctx, int* fault) {
    if (!fn || !ctx) {
        mark(fault, 1);
        return -1;
    }
    mark(fault, 0);
    __try {
        return ((Destroy2Fn)fn)(ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mark(fault, 1);
        return -1;
    }
}

int seh_adl_count(void* fn, void* ctx, int* count, int* fault) {
    if (!fn || !ctx || !count) {
        mark(fault, 1);
        return -1;
    }
    mark(fault, 0);
    __try {
        return ((CountFn)fn)(ctx, count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mark(fault, 1);
        return -1;
    }
}

int seh_adl_pmlog(void* fn, void* ctx, int adapter, void* out, int* fault) {
    if (!fn || !ctx || !out) {
        mark(fault, 1);
        return -1;
    }
    mark(fault, 0);
    __try {
        return ((PmLogFn)fn)(ctx, adapter, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mark(fault, 1);
        return -1;
    }
}

int seh_adl_create1(void* fn, void* (*alloc)(int), int flags, int* fault) {
    if (!fn || !alloc) {
        mark(fault, 1);
        return -1;
    }
    mark(fault, 0);
    __try {
        return ((Create1Fn)fn)(alloc, flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mark(fault, 1);
        return -1;
    }
}

int seh_adl_destroy1(void* fn, int* fault) {
    if (!fn) {
        mark(fault, 1);
        return -1;
    }
    mark(fault, 0);
    __try {
        return ((Destroy1Fn)fn)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mark(fault, 1);
        return -1;
    }
}

int seh_adl_od5_temp(void* fn, int adapter, int thermal, void* out, int* fault) {
    if (!fn || !out) {
        mark(fault, 1);
        return -1;
    }
    mark(fault, 0);
    __try {
        return ((Od5TempFn)fn)(adapter, thermal, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mark(fault, 1);
        return -1;
    }
}

int seh_adl_od5_act(void* fn, int adapter, void* out, int* fault) {
    if (!fn || !out) {
        mark(fault, 1);
        return -1;
    }
    mark(fault, 0);
    __try {
        return ((Od5ActFn)fn)(adapter, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mark(fault, 1);
        return -1;
    }
}
