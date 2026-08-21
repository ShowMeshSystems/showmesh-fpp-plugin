#pragma once

#include <clocale>
#include <cstdlib>
#include <locale.h>

namespace showmesh {

// Forces POSIX ("C") numeric parsing and formatting for the calling
// thread regardless of any locale another component of fppd has set.
// strtod and snprintf's "%e" honor LC_NUMERIC; under a comma-decimal
// locale they silently misparse or emit invalid JSON. If newlocale/
// uselocale are unavailable, this fails to build rather than silently
// falling back to the ambient locale.
class CLocaleGuard {
 public:
    CLocaleGuard() : previous_(uselocale(cLocale())) {}
    ~CLocaleGuard() { uselocale(previous_); }
    CLocaleGuard(const CLocaleGuard&) = delete;
    CLocaleGuard& operator=(const CLocaleGuard&) = delete;

 private:
    static locale_t cLocale() {
        // Created once and reused for the process's lifetime; uselocale
        // only swaps the calling thread's active locale, it does not
        // consume or need a fresh locale_t per call.
        static locale_t loc = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(0));
        if (loc == static_cast<locale_t>(0)) {
            std::abort();
        }
        return loc;
    }

    locale_t previous_;
};

}  // namespace showmesh
