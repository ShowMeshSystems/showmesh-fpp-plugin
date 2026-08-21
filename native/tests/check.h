#pragma once

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

// A test harness small enough to read in one sitting. The native component
// ships as source compiled on an FPP host, so its tests must build with a
// bare toolchain and nothing fetched.

namespace showmesh_test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failureCount() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back(Case{name, fn}); }
};

inline void reportFailure(const char* file, int line, const std::string& message) {
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, message.c_str());
    ++failureCount();
}

template <typename A, typename B>
std::string describeComparison(const char* expr, const A& got, const B& want) {
    std::ostringstream out;
    out << expr << " = " << got << ", want " << want;
    return out.str();
}

inline int runAll() {
    int failedCases = 0;
    for (const Case& c : registry()) {
        const int before = failureCount();
        c.fn();
        if (failureCount() != before) {
            std::fprintf(stderr, "FAILED %s\n", c.name);
            ++failedCases;
        }
    }
    std::fprintf(stderr, "%zu cases, %d failing, %d assertions failed\n", registry().size(), failedCases,
                 failureCount());
    return failedCases == 0 ? 0 : 1;
}

}  // namespace showmesh_test

#define TEST(name)                                                        \
    static void name();                                                   \
    static ::showmesh_test::Registrar registrar_##name(#name, &name);     \
    static void name()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            ::showmesh_test::reportFailure(__FILE__, __LINE__, std::string(#cond) + " is false"); \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(got, want)                                                                          \
    do {                                                                                             \
        const auto& got_ = (got);                                                                    \
        const auto& want_ = (want);                                                                  \
        if (!(got_ == want_)) {                                                                      \
            ::showmesh_test::reportFailure(__FILE__, __LINE__,                                       \
                                           ::showmesh_test::describeComparison(#got, got_, want_));  \
        }                                                                                            \
    } while (0)

#define CHECK_NE(got, unwanted)                                                                          \
    do {                                                                                                 \
        const auto& got_ = (got);                                                                        \
        const auto& unwanted_ = (unwanted);                                                              \
        if (got_ == unwanted_) {                                                                         \
            ::showmesh_test::reportFailure(__FILE__, __LINE__,                                           \
                                           std::string(#got) + " equals " + #unwanted +                  \
                                               ", want them different");                                 \
        }                                                                                                \
    } while (0)

#define CHECK_NEAR(got, want, tolerance)                                                                 \
    do {                                                                                                 \
        const double got_ = static_cast<double>(got);                                                    \
        const double want_ = static_cast<double>(want);                                                  \
        const double tol_ = static_cast<double>(tolerance);                                              \
        if (!(got_ >= want_ - tol_ && got_ <= want_ + tol_)) {                                           \
            ::showmesh_test::reportFailure(__FILE__, __LINE__,                                           \
                                           ::showmesh_test::describeComparison(#got, got_, want_));      \
        }                                                                                                \
    } while (0)
