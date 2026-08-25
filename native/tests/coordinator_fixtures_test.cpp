// Runs the coordinator's frozen contract fixtures
// (tests/fixtures/fpp/canonicalization.json, tests/fixtures/fpp/entry-key.json)
// against this repository's own canonicalization and entry-key derivation.
// See tests/fixtures/fpp/README.md for what these files are and where they
// came from: they are pasted literals from the coordinator repository's Go
// reference implementation, not generated here, and this test is the only
// thing in this repository that proves the two sides still agree.
#include "showmesh/json.h"
#include "showmesh/playlist_identity.h"
#include "showmesh/sha256.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "check.h"

using showmesh::deriveEntryKey;
using showmesh::EntryIdentity;
using showmesh::sha256Hex;
using showmesh::json::CanonicalResult;
using showmesh::json::ParseResult;
using showmesh::json::Value;
using showmesh::json::canonicalize;
using showmesh::json::parse;

namespace {

// Test sources build with cwd at native/ (make -C native test), the same
// directory the .d dependency files and build/ output live relative to.
const char* kCanonicalizationFixturePath = "tests/fixtures/fpp/canonicalization.json";
const char* kEntryKeyFixturePath = "tests/fixtures/fpp/entry-key.json";

std::string readFileOrFail(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        ::showmesh_test::reportFailure(__FILE__, __LINE__, std::string("could not open fixture file ") + path);
        return std::string();
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const Value* findMember(const Value& object, const std::string& name) {
    for (const auto& m : object.members()) {
        if (m.first == name) return &m.second;
    }
    return nullptr;
}

}  // namespace

TEST(CppCanonicalizationMatchesCoordinatorFixtures) {
    std::string raw = readFileOrFail(kCanonicalizationFixturePath);
    if (raw.empty()) return;

    ParseResult fixture = parse(raw);
    CHECK(fixture.ok);
    if (!fixture.ok) return;

    const Value* cases = findMember(fixture.value, "cases");
    CHECK(cases != nullptr);
    if (cases == nullptr) return;
    CHECK(!cases->items().empty());

    for (const Value& c : cases->items()) {
        std::string name;
        std::string input;
        std::string expectedCanonical;
        std::string expectedSha256;
        bool expectError = false;
        if (const Value* v = findMember(c, "name")) name = v->string();
        if (const Value* v = findMember(c, "input")) input = v->string();
        if (const Value* v = findMember(c, "expectedCanonical")) expectedCanonical = v->string();
        if (const Value* v = findMember(c, "expectedSha256")) expectedSha256 = v->string();
        if (const Value* v = findMember(c, "expectError")) expectError = v->boolean();

        CanonicalResult r = canonicalize(input);
        if (expectError) {
            if (r.ok) {
                ::showmesh_test::reportFailure(
                    __FILE__, __LINE__,
                    "canonicalization case \"" + name + "\" was expected to fail but produced " + r.text);
            }
            continue;
        }
        if (!r.ok) {
            ::showmesh_test::reportFailure(
                __FILE__, __LINE__, "canonicalization case \"" + name + "\" failed to canonicalize: " + r.error);
            continue;
        }
        CHECK_EQ(r.text, expectedCanonical);
        CHECK_EQ(sha256Hex(r.text), expectedSha256);
    }
}

TEST(CppEntryKeyMatchesCoordinatorFixtures) {
    std::string raw = readFileOrFail(kEntryKeyFixturePath);
    if (raw.empty()) return;

    ParseResult fixture = parse(raw);
    CHECK(fixture.ok);
    if (!fixture.ok) return;

    const Value* cases = findMember(fixture.value, "cases");
    CHECK(cases != nullptr);
    if (cases == nullptr) return;
    CHECK(!cases->items().empty());

    for (const Value& c : cases->items()) {
        std::string name;
        std::string expectedCanonicalKeyObject;
        std::string expectedEntryKey;
        EntryIdentity id;
        if (const Value* v = findMember(c, "name")) name = v->string();
        if (const Value* v = findMember(c, "expectedCanonicalKeyObject")) expectedCanonicalKeyObject = v->string();
        if (const Value* v = findMember(c, "expectedEntryKey")) expectedEntryKey = v->string();
        if (const Value* identity = findMember(c, "identity")) {
            if (const Value* v = findMember(*identity, "instanceUuid")) id.instanceUuid = v->string();
            if (const Value* v = findMember(*identity, "playlistName")) id.playlistName = v->string();
            if (const Value* v = findMember(*identity, "playlistHash")) id.playlistHash = v->string();
            if (const Value* v = findMember(*identity, "section")) id.section = v->string();
            if (const Value* v = findMember(*identity, "position")) id.position = static_cast<int>(v->number());
        }

        CHECK_EQ(deriveEntryKey(id), expectedEntryKey);

        // Independently canonicalize the same five-member object so a
        // canonicalization bug is distinguishable from a hashing bug, the
        // same way the coordinator's own fixtures_test.go checks it.
        std::vector<Value::Member> members;
        members.emplace_back("instanceUuid", Value::makeString(id.instanceUuid));
        members.emplace_back("playlistHash", Value::makeString(id.playlistHash));
        members.emplace_back("playlistName", Value::makeString(id.playlistName));
        members.emplace_back("position", Value::makeNumber(static_cast<double>(id.position)));
        members.emplace_back("section", Value::makeString(id.section));
        CanonicalResult canon = canonicalize(Value::makeObject(std::move(members)));
        if (!canon.ok) {
            ::showmesh_test::reportFailure(
                __FILE__, __LINE__, "entry-key case \"" + name + "\" failed to canonicalize key object: " + canon.error);
            continue;
        }
        CHECK_EQ(canon.text, expectedCanonicalKeyObject);
        CHECK_EQ(sha256Hex(canon.text), expectedEntryKey);
    }
}
