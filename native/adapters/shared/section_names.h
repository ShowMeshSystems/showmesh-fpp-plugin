#pragma once

#include <string>

// FPP's playlist runtime reports its section using the spellings
// Playlist.cpp hard-codes into m_currentSectionStr: "LeadIn",
// "MainPlaylist", "LeadOut", or "New" before a playlist has started. The
// playlist definition file, and every entry key derived from it on the
// coordinator side (pkg/fppidentity.DefinitionSections in the coordinator
// repository), spells the same three sections as JSON member names:
// "leadIn", "mainPlaylist", "leadOut". That lowercase-first spelling is
// the frozen canonical one, so this maps FPP's runtime string to it once,
// at the adapter boundary, before the section reaches the entry key, the
// wire "section" field, or the current-entry lookup.
//
// A section this function does not recognize, including "New" and the
// empty string, passes through unchanged: inventing a new wire value for
// "unavailable" would be a change to a frozen contract, and the
// coordinator's existing unknown-entry outcome is already the visible,
// safe result for a section it cannot match.

namespace showmesh {
namespace adapter {

inline std::string canonicalPlaylistSection(const std::string& runtimeSection) {
    if (runtimeSection == "LeadIn") return "leadIn";
    if (runtimeSection == "MainPlaylist") return "mainPlaylist";
    if (runtimeSection == "LeadOut") return "leadOut";
    return runtimeSection;
}

}  // namespace adapter
}  // namespace showmesh
