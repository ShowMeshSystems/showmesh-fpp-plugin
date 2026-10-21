#include "showmesh/command_targets.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "check.h"
#include "showmesh/atomic_write.h"

using showmesh::CommandTargetLists;
using showmesh::ConfigObjectSummary;
using showmesh::HttpRequest;
using showmesh::HttpResponse;
using showmesh::HttpTransport;
using showmesh::parseConfigObjectsList;
using showmesh::renderCommandTargetOptions;

namespace {

class FakeTransport : public HttpTransport {
 public:
    std::map<std::string, HttpResponse> answers;
    std::vector<HttpRequest> requests;

    HttpResponse get(const HttpRequest& request) override {
        requests.push_back(request);
        const auto it = answers.find(request.url);
        return it == answers.end() ? HttpResponse{} : it->second;
    }
    HttpResponse post(const HttpRequest&) override { return HttpResponse{}; }
    HttpResponse put(const HttpRequest&) override { return HttpResponse{}; }
};

class FixedCredential : public showmesh::CredentialSource {
 public:
    bool token(std::string* out, std::string*) override {
        *out = "secret";
        return true;
    }
    void invalidate() override {}
};

HttpResponse ok(const std::string& body) { return HttpResponse{true, 200, body, std::string()}; }

std::string listBody(const std::string& objects) {
    return R"({"serverTime":"2026-10-21T00:00:00Z","kind":"show.macro","objects":[)" + objects + "]}";
}

std::string tempPath() {
    char dir[] = "/tmp/showmesh-command-targets-XXXXXX";
    const char* made = mkdtemp(dir);
    return made == nullptr ? std::string() : std::string(made) + "/command-targets.json";
}

const std::vector<ConfigObjectSummary> kShows = {{"spring", "Spring Show", ""}, {"blank", "", ""}};

}  // namespace

TEST(AConfigObjectsListParsesIdLabelAndShow) {
    std::vector<ConfigObjectSummary> list;
    CHECK(parseConfigObjectsList(listBody(R"({"id":"m1","label":"Open","show":"spring","currentRevision":3,)"
                                          R"("updatedAt":"2026-10-21T00:00:00Z"},{"label":"no id"},)"
                                          R"({"id":"m2"})"),
                                 &list));
    CHECK_EQ(list.size(), static_cast<std::size_t>(2));
    CHECK_EQ(list[0].id, std::string("m1"));
    CHECK_EQ(list[0].label, std::string("Open"));
    CHECK_EQ(list[0].show, std::string("spring"));
    CHECK_EQ(list[1].id, std::string("m2"));
    CHECK_EQ(list[1].label, std::string(""));
}

TEST(AnAnswerThatIsNotAListIsRefused) {
    std::vector<ConfigObjectSummary> list;
    CHECK(!parseConfigObjectsList("not json", &list));
    CHECK(!parseConfigObjectsList(R"({"objects":{}})", &list));
    CHECK(!parseConfigObjectsList(R"([])", &list));
}

TEST(OptionsAreOrderedByVisibleTextAndNameTheirShow) {
    const std::vector<ConfigObjectSummary> macros = {
        {"z1", "Beta", "spring"}, {"a1", "Alpha", "missing"}, {"m1", "", ""}, {"b1", "Gamma", "blank"}};
    CHECK_EQ(renderCommandTargetOptions(&macros, &kShows, "macros"),
             std::string(R"j({"a1":"Alpha","z1":"Beta (Spring Show)","b1":"Gamma (blank)","m1":"m1"})j"));
}

TEST(TheShowIsOmittedWhenNoShowListWasFetched) {
    const std::vector<ConfigObjectSummary> macros = {{"z1", "Beta", "spring"}};
    CHECK_EQ(renderCommandTargetOptions(&macros, nullptr, "macros"), std::string(R"({"z1":"Beta"})"));
}

TEST(VisibleTextIsHtmlEscaped) {
    const std::vector<ConfigObjectSummary> macros = {{"x", "<b>\"Tom\" & 'Jerry'</b>", "spring"}};
    const std::vector<ConfigObjectSummary> shows = {{"spring", "A&B", ""}};
    CHECK_EQ(renderCommandTargetOptions(&macros, &shows, "macros"),
             std::string(R"j({"x":"&lt;b&gt;&quot;Tom&quot; &amp; &#39;Jerry&#39;&lt;/b&gt; (A&amp;B)"})j"));
}

TEST(IdsOutsideTheAllowedCharactersAreDropped) {
    const std::vector<ConfigObjectSummary> macros = {{"ok.id_1:x-y", "Kept", ""},
                                                     {"bad id", "Space", ""},
                                                     {"bad'id", "Quote", ""},
                                                     {"bad<id>", "Angle", ""},
                                                     {"", "Empty", ""},
                                                     {"ok.id_1:x-y", "Duplicate", ""}};
    CHECK_EQ(renderCommandTargetOptions(&macros, &kShows, "macros"), std::string(R"({"ok.id_1:x-y":"Kept"})"));
}

TEST(NeverFetchedListsAskForPairing) {
    CHECK_EQ(renderCommandTargetOptions(nullptr, nullptr, "macros"),
             std::string(R"({"":"No macros found. Pair this FPP with ShowMesh on the ShowMesh plugin page."})"));
    CHECK_EQ(renderCommandTargetOptions(nullptr, &kShows, "actions"),
             std::string(R"({"":"No actions found. Pair this FPP with ShowMesh on the ShowMesh plugin page."})"));
}

TEST(FetchedEmptyListsSayNoneAreSetUp) {
    const std::vector<ConfigObjectSummary> empty;
    const std::vector<ConfigObjectSummary> allDropped = {{"bad id", "x", ""}};
    CHECK_EQ(renderCommandTargetOptions(&empty, &kShows, "macros"),
             std::string(R"({"":"No macros are set up in ShowMesh."})"));
    CHECK_EQ(renderCommandTargetOptions(&allDropped, &kShows, "actions"),
             std::string(R"({"":"No actions are set up in ShowMesh."})"));
}

TEST(ARefreshFetchesAllThreeListsAndSavesThem) {
    const std::string path = tempPath();
    FakeTransport transport;
    FixedCredential credential;
    transport.answers["http://c/api/v1/config/show.macro"] = ok(listBody(R"({"id":"m1","label":"Open","show":"s"})"));
    transport.answers["http://c/api/v1/config/show.action"] = ok(listBody(""));
    transport.answers["http://c/api/v1/config/show"] = ok(listBody(R"({"id":"s","label":"Spring","show":""})"));
    CommandTargetLists lists(path);
    CHECK_EQ(lists.macrosBody().find("Pair this FPP") != std::string::npos, true);
    CHECK_EQ(lists.refresh(&transport, &credential, "http://c"), std::string());
    CHECK_EQ(transport.requests.size(), static_cast<std::size_t>(3));
    CHECK_EQ(transport.requests[0].bearerToken, std::string("secret"));
    CHECK_EQ(lists.macrosBody(), std::string(R"j({"m1":"Open (Spring)"})j"));
    CHECK_EQ(lists.actionsBody(), std::string(R"({"":"No actions are set up in ShowMesh."})"));

    CommandTargetLists reloaded(path);
    CHECK_EQ(reloaded.macrosBody(), std::string(R"j({"m1":"Open (Spring)"})j"));
    CHECK_EQ(reloaded.actionsBody(), std::string(R"({"":"No actions are set up in ShowMesh."})"));
}

TEST(AFailedFetchKeepsTheLastGoodList) {
    const std::string path = tempPath();
    FakeTransport transport;
    FixedCredential credential;
    transport.answers["http://c/api/v1/config/show.macro"] = ok(listBody(R"({"id":"m1","label":"Open","show":""})"));
    transport.answers["http://c/api/v1/config/show.action"] = ok(listBody(R"({"id":"a1","label":"Go","show":""})"));
    transport.answers["http://c/api/v1/config/show"] = ok(listBody(""));
    CommandTargetLists lists(path);
    CHECK_EQ(lists.refresh(&transport, &credential, "http://c"), std::string());

    transport.answers["http://c/api/v1/config/show.macro"] = HttpResponse{true, 503, "", ""};
    transport.answers["http://c/api/v1/config/show.action"] = ok("garbage");
    transport.answers.erase("http://c/api/v1/config/show");
    CHECK(!lists.refresh(&transport, &credential, "http://c").empty());
    CHECK_EQ(lists.macrosBody(), std::string(R"({"m1":"Open"})"));
    CHECK_EQ(lists.actionsBody(), std::string(R"({"a1":"Go"})"));
    CHECK_EQ(CommandTargetLists(path).macrosBody(), std::string(R"({"m1":"Open"})"));
}

TEST(TheCommandTargetRoutesSitBesideTheBrightnessRoute) {
    CHECK_EQ(std::string(showmesh::kCommandTargetMacrosPath), std::string("/showmesh/macros"));
    CHECK_EQ(std::string(showmesh::kCommandTargetActionsPath), std::string("/showmesh/actions"));
    CHECK_EQ(std::string(showmesh::kCommandTargetMacrosLanPath), std::string("/api/plugin-apis/showmesh/macros"));
    CHECK_EQ(std::string(showmesh::kCommandTargetActionsLanPath), std::string("/api/plugin-apis/showmesh/actions"));
}
