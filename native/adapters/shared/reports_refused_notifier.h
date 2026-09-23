#pragma once

#include <string>

#include "Warnings.h"
#include "showmesh/runtime.h"

// Raises and clears the reports-refused notice through FPP's own
// notification centre. Copies WarningHolderMismatchNotifier's pattern
// rather than sharing it: the two notices have unrelated triggers and
// unrelated lifetimes.

namespace showmesh {
namespace adapter {

class WarningHolderReportsRefusedNotifier : public ReportsRefusedNotifier {
 public:
    void raiseRefused(int id, const std::string& message) override {
        WarningHolder::AddWarningTimeout(-1, id, message, {}, kPluginName);
    }

    void clearRefused(int id, const std::string& message) override {
        WarningHolder::RemoveWarning(id, message, kPluginName);
    }
};

}  // namespace adapter
}  // namespace showmesh
