#include "showmesh/safe_ceiling_setting.h"

#include <string>

#include "check.h"

using showmesh::parseSafeCeilingSetting;

TEST(AWellFormedValueParses) {
    int percent = -1;
    CHECK(parseSafeCeilingSetting("50", &percent));
    CHECK_EQ(percent, 50);
}

TEST(ZeroIsAcceptedAsAnExplicitOperatorChoice) {
    int percent = -1;
    CHECK(parseSafeCeilingSetting("0", &percent));
    CHECK_EQ(percent, 0);
}

TEST(OneHundredIsAcceptedAtTheTopOfTheRange) {
    int percent = -1;
    CHECK(parseSafeCeilingSetting("100", &percent));
    CHECK_EQ(percent, 100);
}

TEST(EmptyInputIsRejected) {
    int percent = -1;
    CHECK(!parseSafeCeilingSetting("", &percent));
}

TEST(NonNumericInputIsRejected) {
    int percent = -1;
    CHECK(!parseSafeCeilingSetting("abc", &percent));
    CHECK(!parseSafeCeilingSetting("50%", &percent));
    CHECK(!parseSafeCeilingSetting("fifty", &percent));
}

TEST(NegativeInputIsRejected) {
    int percent = -1;
    CHECK(!parseSafeCeilingSetting("-1", &percent));
    CHECK(!parseSafeCeilingSetting("-50", &percent));
}

TEST(AboveOneHundredIsRejected) {
    int percent = -1;
    CHECK(!parseSafeCeilingSetting("101", &percent));
    CHECK(!parseSafeCeilingSetting("1000", &percent));
}

TEST(LeadingAndTrailingWhitespaceIsTrimmed) {
    int percent = -1;
    CHECK(parseSafeCeilingSetting("  50", &percent));
    CHECK_EQ(percent, 50);
    percent = -1;
    CHECK(parseSafeCeilingSetting("50  ", &percent));
    CHECK_EQ(percent, 50);
    percent = -1;
    CHECK(parseSafeCeilingSetting("\t 25 \t", &percent));
    CHECK_EQ(percent, 25);
}

TEST(WhitespaceOnlyInputIsRejected) {
    int percent = -1;
    CHECK(!parseSafeCeilingSetting("   ", &percent));
}

TEST(InternalWhitespaceIsRejected) {
    int percent = -1;
    CHECK(!parseSafeCeilingSetting("5 0", &percent));
}

TEST(AFractionalValueIsRejected) {
    int percent = -1;
    CHECK(!parseSafeCeilingSetting("50.5", &percent));
}

TEST(TrailingGarbageAfterTheNumberIsRejected) {
    int percent = -1;
    CHECK(!parseSafeCeilingSetting("50abc", &percent));
}
