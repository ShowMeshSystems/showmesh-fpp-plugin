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

// Decodes lowercase hex bytes, as a fixture case's inputHex field carries a
// byte sequence a JSON string cannot hold (invalid UTF-8). Returns false on
// odd length or a non-hex digit, which a case must never hit: those failures
// mean the fixture itself is malformed.
bool hexDecode(const std::string& hex, std::string* out) {
    if (hex.size() % 2 != 0) return false;
    out->clear();
    out->reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int nibbles[2];
        for (int k = 0; k < 2; ++k) {
            const char c = hex[i + static_cast<std::size_t>(k)];
            if (c >= '0' && c <= '9') {
                nibbles[k] = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                nibbles[k] = c - 'a' + 10;
            } else {
                return false;
            }
        }
        out->push_back(static_cast<char>((nibbles[0] << 4) | nibbles[1]));
    }
    return true;
}

// A case supplies exactly one of input or inputHex; inputHex exists because
// a JSON string cannot carry a byte sequence that is not valid UTF-8.
// Returns false, with *errorOut set, on neither/both present or malformed
// hex, rather than silently resolving to an empty string: a misspelled
// field name must fail loudly, not canonicalize nothing.
bool resolveCaseInput(const Value& c, std::string* input, std::string* errorOut) {
    const Value* inputField = findMember(c, "input");
    const Value* inputHexField = findMember(c, "inputHex");
    if ((inputField == nullptr) == (inputHexField == nullptr)) {
        *errorOut = "must supply exactly one of input or inputHex";
        return false;
    }
    if (inputField != nullptr) {
        *input = inputField->string();
        return true;
    }
    if (!hexDecode(inputHexField->string(), input)) {
        *errorOut = "has malformed inputHex";
        return false;
    }
    return true;
}

}  // namespace

// Exercises resolveCaseInput directly against synthetic cases rather than
// the vendored fixture, which is frozen and always well-formed: nothing in
// it would ever call this guard, so a fixture-only suite would let the
// guard rot away without any test noticing.
TEST(FixtureCaseMustSupplyExactlyOneOfInputOrInputHex) {
    std::string input, error;

    Value neither = Value::makeObject({});
    CHECK(!resolveCaseInput(neither, &input, &error));

    std::vector<Value::Member> both;
    both.emplace_back("input", Value::makeString("{}"));
    both.emplace_back("inputHex", Value::makeString("7b7d"));
    CHECK(!resolveCaseInput(Value::makeObject(std::move(both)), &input, &error));

    std::vector<Value::Member> justInput;
    justInput.emplace_back("input", Value::makeString("{}"));
    CHECK(resolveCaseInput(Value::makeObject(std::move(justInput)), &input, &error));
    CHECK_EQ(input, "{}");

    std::vector<Value::Member> justHex;
    justHex.emplace_back("inputHex", Value::makeString("7b7d"));
    CHECK(resolveCaseInput(Value::makeObject(std::move(justHex)), &input, &error));
    CHECK_EQ(input, "{}");

    std::vector<Value::Member> badHex;
    badHex.emplace_back("inputHex", Value::makeString("zz"));
    CHECK(!resolveCaseInput(Value::makeObject(std::move(badHex)), &input, &error));
}

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
        std::string expectedCanonical;
        std::string expectedSha256;
        bool expectError = false;
        if (const Value* v = findMember(c, "name")) name = v->string();
        if (const Value* v = findMember(c, "expectedCanonical")) expectedCanonical = v->string();
        if (const Value* v = findMember(c, "expectedSha256")) expectedSha256 = v->string();
        if (const Value* v = findMember(c, "expectError")) expectError = v->boolean();

        // A case supplies exactly one of input or inputHex; inputHex exists
        // because a JSON string cannot carry a byte sequence that is not
        // valid UTF-8. Refusing neither-or-both here, rather than silently
        // canonicalizing an empty string, is what stops a misspelled field
        // name from passing quietly.
        const Value* inputField = findMember(c, "input");
        const Value* inputHexField = findMember(c, "inputHex");
        if ((inputField == nullptr) == (inputHexField == nullptr)) {
            ::showmesh_test::reportFailure(
                __FILE__, __LINE__,
                "canonicalization case \"" + name + "\" must supply exactly one of input or inputHex");
            continue;
        }
        std::string input;
        if (inputField != nullptr) {
            input = inputField->string();
        } else if (!hexDecode(inputHexField->string(), &input)) {
            ::showmesh_test::reportFailure(
                __FILE__, __LINE__, "canonicalization case \"" + name + "\" has malformed inputHex");
            continue;
        }

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
