#pragma once

#include <clocale>
#include <cstdlib>
#include <locale.h>

namespace showmesh {

// Forces POSIX ("C") numeric parsing and formatting for the calling
// thread regardless of any locale another component of fppd has set.
// strtod and snprintf's "%e" honor LC_NUMERIC; under a comma-decimal
// locale they silently misparse or emit invalid JSON.
//
// This runs on fppd's output thread mid-show. If newlocale/uselocale
// cannot produce the C locale, the guard reports that through ok()
// instead of aborting the process: the caller must fail its own
// operation rather than proceed under whatever locale happened to be
// ambient, or crash a running show over it.
class CLocaleGuard {
 public:
    CLocaleGuard() {
        locale_t loc = cLocale();
        if (loc == static_cast<locale_t>(0)) {
            ok_ = false;
            return;
        }
        previous_ = uselocale(loc);
        ok_ = true;
    }
    ~CLocaleGuard() {
        if (ok_) uselocale(previous_);
    }
    CLocaleGuard(const CLocaleGuard&) = delete;
    CLocaleGuard& operator=(const CLocaleGuard&) = delete;

    bool ok() const { return ok_; }

 private:
    static locale_t cLocale() {
        // Created once and reused for the process's lifetime; uselocale
        // only swaps the calling thread's active locale, it does not
        // consume or need a fresh locale_t per call.
        static locale_t loc = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(0));
        return loc;
    }

    locale_t previous_ = static_cast<locale_t>(0);
    bool ok_ = false;
};

}  // namespace showmesh
