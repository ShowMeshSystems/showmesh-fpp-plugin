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
    CLocaleGuard() : CLocaleGuard(cLocale(), &uselocale) {}
    ~CLocaleGuard() {
        if (ok_) switchFn_(previous_);
    }
    CLocaleGuard(const CLocaleGuard&) = delete;
    CLocaleGuard& operator=(const CLocaleGuard&) = delete;

    bool ok() const { return ok_; }

 private:
    // Lets locale_guard_test.cpp construct a guard against an injected
    // switch function that reports failure, which the real uselocale(3)
    // cannot be made to do portably without invoking undefined behavior.
    friend struct CLocaleGuardTestHook;

    CLocaleGuard(locale_t loc, locale_t (*switchFn)(locale_t)) : switchFn_(switchFn) {
        if (loc == static_cast<locale_t>(0)) {
            ok_ = false;
            return;
        }
        // uselocale returns (locale_t)0 on failure and never returns it on
        // success (LC_GLOBAL_LOCALE and any real locale_t are non-null), so
        // this is the only correct success signal.
        previous_ = switchFn(loc);
        ok_ = (previous_ != static_cast<locale_t>(0));
    }

    static locale_t cLocale() {
        // Created once and reused for the process's lifetime; uselocale
        // only swaps the calling thread's active locale, it does not
        // consume or need a fresh locale_t per call.
        static locale_t loc = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(0));
        return loc;
    }

    locale_t (*switchFn_)(locale_t) = &uselocale;
    locale_t previous_ = static_cast<locale_t>(0);
    bool ok_ = false;
};

}  // namespace showmesh
