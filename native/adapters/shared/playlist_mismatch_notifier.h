#pragma once

#include <string>

#include "Warnings.h"
#include "showmesh/runtime.h"

// Raises and clears the mid-show playlist-mismatch notice through FPP's
// own notification centre. Shared by both adapters: WarningHolder's
// AddWarningTimeout/RemoveWarning signatures are identical on FPP 9 and
// FPP 10.
//
// A pure pass-through by design: the id and message come from the caller
// (ShowMeshRuntime, via showmesh::ShowMesh_PlaylistMismatch and
// showmesh::kPlaylistMismatchInstruction) rather than being fixed here, so
// there is exactly one place in this repository that could make the
// raise and the clear disagree, and it is not this file.

namespace showmesh {
namespace adapter {

class WarningHolderMismatchNotifier : public PlaylistMismatchNotifier {
 public:
    void raiseMismatch(int id, const std::string& message) override {
        // No timeout: FPP does not expire this on its own, only an exact
        // clearMismatch() call does. The plugin name is passed explicitly
        // so RemoveWarning's exact-triple match below can never confuse
        // this notice with a differently sourced warning that happens to
        // share an id and message.
        WarningHolder::AddWarningTimeout(-1, id, message, {}, kPluginName);
    }

    void clearMismatch(int id, const std::string& message) override {
        WarningHolder::RemoveWarning(id, message, kPluginName);
    }
};

}  // namespace adapter
}  // namespace showmesh
