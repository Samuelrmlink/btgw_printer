/* Native-test wrapper around the SDK's pex.h. The fake host compiles modules
 * for the build machine (gcc -m32, so pointers still fit the uint32_t words
 * pex_call passes), and the SDK's export/weak-import macros emit ARM
 * metadata sections that mean nothing here. They are replaced:
 *   PEX_EXPORT(fn)        nothing; the test registers exports with fh_export()
 *   PEX_WEAK_IMPORT(sym)  nothing
 *   pex_avail(sym)        fh_avail("sym"): false once a test disables a group
 *   PEX_META_STR          nothing */
#ifndef FH_PEX_WRAP_H
#define FH_PEX_WRAP_H
#include_next "pex.h"
#undef PEX_EXPORT
#undef PEX_WEAK_IMPORT
#undef pex_avail
#undef PEX_META_STR
#define PEX_EXPORT(fn) extern int fh__export_unused
#define PEX_WEAK_IMPORT(sym) extern int fh__weak_unused
#define PEX_META_STR(key, val) extern int fh__meta_unused
int fh_avail(const char *sym);
#define pex_avail(sym) fh_avail(#sym)
#endif
