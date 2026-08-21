#pragma once

// jsoncpp installs its header at a different path depending on the
// distribution. FPP 10 has its own single include for this; FPP 9 does the
// check inline in each header. The adapters need the same names on both,
// and this file is the only place that difference is expressed.

#if __has_include(<jsoncpp/json/json.h>)
#include <jsoncpp/json/json.h>
#elif __has_include(<json/json.h>)
#include <json/json.h>
#else
#error "jsoncpp headers were not found; FPP itself requires them"
#endif
