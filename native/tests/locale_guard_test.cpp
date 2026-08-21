#include "showmesh/locale_guard.h"

#include <clocale>
#include <cstdio>
#include <string>

#include "check.h"

namespace showmesh {

// Grants this translation unit access to CLocaleGuard's test-only
// constructor overload, which accepts an injected switch function.
struct CLocaleGuardTestHook {
    static CLocaleGuard makeWithSwitch(locale_t loc, locale_t (*switchFn)(locale_t)) {
        return CLocaleGuard(loc, switchFn);
    }
};

}  // namespace showmesh

using showmesh::CLocaleGuard;
using showmesh::CLocaleGuardTestHook;

namespace {

locale_t alwaysFails(locale_t) { return static_cast<locale_t>(0); }

std::string decimalPointNow() {
    // localeconv() reports the calling thread's active LC_NUMERIC facet
    // under uselocale, which is exactly what CLocaleGuard manipulates.
    return std::string(std::localeconv()->decimal_point);
}

}  // namespace

TEST(GuardSucceedsAndForcesTheCNumericLocale) {
    const CLocaleGuard guard;
    CHECK(guard.ok());
    CHECK_EQ(decimalPointNow(), ".");
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", 1.5);
    CHECK_EQ(std::string(buf), "1.5");
}

// Regression for the ok_ bug: the constructor must derive ok() from
// whether the switch to the C locale actually succeeded, not assume
// success once a non-null C locale_t exists. A guard that reports ok()
// while the ambient locale never changed lets callers proceed as if
// LC_NUMERIC were "C" when it is not.
TEST(GuardReportsFailureWhenTheSwitchFails) {
    locale_t cLoc = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(0));
    CHECK(cLoc != static_cast<locale_t>(0));
    {
        CLocaleGuard guard = CLocaleGuardTestHook::makeWithSwitch(cLoc, &alwaysFails);
        CHECK(!guard.ok());
    }
    freelocale(cLoc);
}

// Regression for the restore bug: destruction must reinstate the exact
// locale object that was active before construction, not merely query the
// current one (a bare uselocale(0) call is a no-op query, not a restore,
// and would leave the thread parked on the guard's C locale).
TEST(GuardRestoresThePreviousLocaleOnDestruction) {
    locale_t custom = static_cast<locale_t>(0);
    for (const char* candidate : {"de_DE.UTF-8", "de_DE", "de_DE.ISO8859-1"}) {
        custom = newlocale(LC_NUMERIC_MASK, candidate, static_cast<locale_t>(0));
        if (custom != static_cast<locale_t>(0)) break;
    }
    if (custom == static_cast<locale_t>(0)) {
        const char* required = std::getenv("SHOWMESH_REQUIRE_LOCALE_TEST");
        if (required != nullptr && required[0] != '\0' && required[0] != '0') {
            ::showmesh_test::reportFailure(
                __FILE__, __LINE__,
                "no de_DE locale (tried de_DE.UTF-8, de_DE, de_DE.ISO8859-1) is installed, and "
                "SHOWMESH_REQUIRE_LOCALE_TEST demands one");
            return;
        }
        std::fprintf(stderr,
                     "SKIP GuardRestoresThePreviousLocaleOnDestruction: no de_DE locale installed here, and "
                     "SHOWMESH_REQUIRE_LOCALE_TEST is not set\n");
        return;
    }

    const locale_t previousGlobal = uselocale(custom);
    {
        const CLocaleGuard guard;
        CHECK(guard.ok());
        CHECK_EQ(decimalPointNow(), ".");
    }
    const locale_t afterActive = uselocale(static_cast<locale_t>(0));
    CHECK_EQ(afterActive, custom);

    uselocale(previousGlobal);
    freelocale(custom);
}
