/* SPDX-License-Identifier: GPL-2.0-only */
// Minimal unit-test harness: TEST(name) { ... } with CHECK/CHECK_EQ.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

struct UTest {
    const char* name;
    void (*fn)();
};
inline std::vector<UTest>& utests() {
    static std::vector<UTest> v;
    return v;
}
inline int& ufailures() {
    static int n = 0;
    return n;
}
struct URegister {
    URegister(const char* name, void (*fn)()) { utests().push_back({name, fn}); }
};

#define TEST(name)                                   \
    static void utest_##name();                      \
    static URegister ureg_##name(#name, utest_##name); \
    static void utest_##name()

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++ufailures();                                                   \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        auto _a = (a);                                                                  \
        auto _b = (b);                                                                  \
        if (!(_a == _b)) {                                                              \
            ++ufailures();                                                              \
            printf("    FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                   (long long)_a, (long long)_b);                                       \
        }                                                                               \
    } while (0)

inline int run_utests() {
    for (const UTest& t : utests()) {
        int before = ufailures();
        t.fn();
        printf("%s %s\n", ufailures() == before ? "ok  " : "FAIL", t.name);
    }
    printf("%d test(s), %d failure(s)\n", int(utests().size()), ufailures());
    return ufailures() ? 1 : 0;
}

#define UTEST_MAIN() \
    int main() { return run_utests(); }
