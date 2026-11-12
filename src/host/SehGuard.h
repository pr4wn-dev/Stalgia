#pragma once

// ADL calls live in a C file so a driver fault can be caught with SEH.
// Every wrapper refuses a NULL function pointer and returns fault=1.

#ifdef __cplusplus
extern "C" {
#endif

int seh_adl_create2(void* fn, void* (*alloc)(int), int flags, void** ctx, int* fault);
int seh_adl_destroy2(void* fn, void* ctx, int* fault);
int seh_adl_count(void* fn, void* ctx, int* count, int* fault);
int seh_adl_pmlog(void* fn, void* ctx, int adapter, void* out, int* fault);
int seh_adl_create1(void* fn, void* (*alloc)(int), int flags, int* fault);
int seh_adl_destroy1(void* fn, int* fault);
int seh_adl_od5_temp(void* fn, int adapter, int thermal, void* out, int* fault);
int seh_adl_od5_act(void* fn, int adapter, void* out, int* fault);

#ifdef __cplusplus
}
#endif
