#include "showmesh/json.h"

#include <string>

#include "check.h"

using showmesh::json::canonicalize;
using showmesh::json::CanonicalResult;
using showmesh::json::formatNumber;
using showmesh::json::parse;

namespace {

std::string canonicalText(const std::string& input) {
    CanonicalResult r = canonicalize(input);
    return r.ok ? r.text : std::string("<error: ") + r.error + ">";
}

std::string formatted(double v) {
    std::string out;
    return formatNumber(v, &out) ? out : std::string("<unrepresentable>");
}

}  // namespace

TEST(CanonicalizationSortsMembersAndStripsWhitespace) {
    CHECK_EQ(canonicalText("{ \"b\": 1, \"a\": 2 }"), "{\"a\":2,\"b\":1}");
    CHECK_EQ(canonicalText("[ 1 , 2 ,  3 ]"), "[1,2,3]");
    CHECK_EQ(canonicalText("{\"nested\":{\"z\":true,\"a\":null}}"), "{\"nested\":{\"a\":null,\"z\":true}}");
}

TEST(MemberNamesSortByUtf16CodeUnitsNotBytes) {
    // U+10000 encodes as the surrogate pair D800 DC00, which sorts before
    // U+E000 in UTF-16 and after it in raw UTF-8 byte order. Byte-order
    // sorting would put these two the other way round.
    const std::string input = "{\"\\ue000\":1,\"\\ud800\\udc00\":2}";
    CHECK_EQ(canonicalText(input), "{\"\xF0\x90\x80\x80\":2,\"\xEE\x80\x80\":1}");
}

TEST(StringsUseTheShortEscapesAndEscapeOnlyWhatMustBe) {
    CHECK_EQ(canonicalText("[\"a\\u0062c\"]"), "[\"abc\"]");
    CHECK_EQ(canonicalText("[\"\\u000b\"]"), "[\"\\u000b\"]");
    CHECK_EQ(canonicalText("[\"tab\\there\"]"), "[\"tab\\there\"]");
    CHECK_EQ(canonicalText("[\"quote\\\" and backslash\\\\\"]"), "[\"quote\\\" and backslash\\\\\"]");
    // A solidus is escapable in JSON but is not escaped on output.
    CHECK_EQ(canonicalText("[\"a\\/b\"]"), "[\"a/b\"]");
}

TEST(NumbersUseEcmascriptFormatting) {
    CHECK_EQ(formatted(0.0), "0");
    CHECK_EQ(formatted(-0.0), "0");
    CHECK_EQ(formatted(1.0), "1");
    CHECK_EQ(formatted(-1.0), "-1");
    CHECK_EQ(formatted(1.5), "1.5");
    CHECK_EQ(formatted(100.0), "100");
    CHECK_EQ(formatted(1e21), "1e+21");
    CHECK_EQ(formatted(1e20), "100000000000000000000");
    CHECK_EQ(formatted(1e-6), "0.000001");
    CHECK_EQ(formatted(1e-7), "1e-7");
    CHECK_EQ(formatted(0.1), "0.1");
    CHECK_EQ(formatted(1.0 / 3.0), "0.3333333333333333");
    CHECK_EQ(formatted(9007199254740992.0), "9007199254740992");
    CHECK_EQ(formatted(1.2345e-10), "1.2345e-10");
}

TEST(CanonicalizationRewritesEquivalentNumberLiterals) {
    CHECK_EQ(canonicalText("[1.0,1e2,1E+2,0.10,-0]"), "[1,100,100,0.1,0]");
}

TEST(DuplicateMemberNamesAreRejected) {
    CanonicalResult r = canonicalize("{\"a\":1,\"a\":2}");
    CHECK(!r.ok);
}

TEST(MalformedJsonIsRejectedRatherThanRepaired) {
    CHECK(!canonicalize("{").ok);
    CHECK(!canonicalize("{\"a\":}").ok);
    CHECK(!canonicalize("[1,]").ok);
    CHECK(!canonicalize("01").ok);
    CHECK(!canonicalize("\"unterminated").ok);
    CHECK(!canonicalize("{\"a\":1} trailing").ok);
    CHECK(!canonicalize("[\"\\ud800\"]").ok);
    CHECK(!canonicalize("").ok);
}

TEST(CanonicalizationIsIdempotent) {
    const std::string once = canonicalText("{\"b\":[3,2,{\"y\":1,\"x\":2}],\"a\":\"text\"}");
    CHECK_EQ(canonicalText(once), once);
}

TEST(MemberOrderDoesNotChangeTheCanonicalForm) {
    CHECK_EQ(canonicalText("{\"a\":1,\"b\":2,\"c\":3}"), canonicalText("{\"c\":3,\"b\":2,\"a\":1}"));
}
