/* A small test harness: T_CASE functions run in order from T_MAIN. */
#ifndef T_H
#define T_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int t_fail, t_checks, t_cases;
static const char *t_name;

#define CHECK(c) do { t_checks++; if (!(c)) { t_fail++; \
    fprintf(stderr, "FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, t_name, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); t_checks++; \
    if (_a != _b) { t_fail++; fprintf(stderr, "FAIL %s:%d [%s]: %s == %s (%lld vs %lld)\n", \
    __FILE__, __LINE__, t_name, #a, #b, _a, _b); } } while (0)
#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); t_checks++; \
    if (!_a || !_b || strcmp(_a, _b)) { t_fail++; fprintf(stderr, "FAIL %s:%d [%s]: %s == \"%s\" (got \"%s\")\n", \
    __FILE__, __LINE__, t_name, #a, _b ? _b : "(null)", _a ? _a : "(null)"); } } while (0)
#define CHECK_HAS(hay, needle) do { const char *_h = (hay); t_checks++; \
    if (!_h || !strstr(_h, needle)) { t_fail++; fprintf(stderr, "FAIL %s:%d [%s]: \"%s\" in \"%s\"\n", \
    __FILE__, __LINE__, t_name, needle, _h ? _h : "(null)"); } } while (0)

#define RUN(fn) do { t_name = #fn; t_cases++; int _f = t_fail; fn(); \
    if (getenv("T_VERBOSE")) { fprintf(stderr, "%s %s\n", t_fail == _f ? "ok  " : "FAIL", #fn); } } while (0)
#define T_DONE() do { printf("%d cases, %d checks, %d failed\n", t_cases, t_checks, t_fail); \
    return t_fail ? 1 : 0; } while (0)
#endif
