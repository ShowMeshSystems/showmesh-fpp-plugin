#include "showmesh/command_targets.h"

#include <algorithm>
#include <set>
#include <utility>

#include "showmesh/atomic_write.h"
#include "showmesh/json.h"

namespace showmesh {

const char* const kCommandTargetMacrosPath = "/showmesh/macros";
const char* const kCommandTargetActionsPath = "/showmesh/actions";
const char* const kCommandTargetMacrosLanPath = "/api/plugin-apis/showmesh/macros";
const char* const kCommandTargetActionsLanPath = "/api/plugin-apis/showmesh/actions";

namespace {

// Large enough for thousands of summaries, and still a bound.
constexpr int kListMaxResponseBytes = 1 << 20;
constexpr int kListTimeoutMillis = 10000;

const json::Value* member(const json::Value& object, const std::string& name) {
    for (const auto& m : object.members()) {
        if (m.first == name) return &m.second;
    }
    return nullptr;
}

std::string stringMember(const json::Value& object, const std::string& name) {
    const json::Value* value = member(object, name);
    return value != nullptr && value->type() == json::Type::kString ? value->string() : std::string();
}

void parseSummaries(const json::Value& items, std::vector<ConfigObjectSummary>* out) {
    for (const json::Value& item : items.items()) {
        if (item.type() != json::Type::kObject) continue;
        const json::Value* id = member(item, "id");
        if (id == nullptr || id->type() != json::Type::kString) continue;
        out->push_back(ConfigObjectSummary{id->string(), stringMember(item, "label"), stringMember(item, "show")});
    }
}

// FPP before 10.3 inserts the text as raw HTML and 10.3 escapes it, so only '<', which could open a tag, is swapped.
std::string withoutTagOpeners(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '<') {
            out += "\xE2\x80\xB9";
        } else {
            out += c;
        }
    }
    return out;
}

// A JSON string literal, or nothing when the text cannot be encoded.
std::optional<std::string> jsonString(const std::string& text) {
    const json::CanonicalResult rendered = json::canonicalize(json::Value::makeString(text));
    if (!rendered.ok) return std::nullopt;
    return rendered.text;
}

std::string placeholder(const std::string& text) {
    const std::optional<std::string> value = jsonString(text);
    return "{\"\":" + value.value_or("\"\"") + "}";
}

json::Value summariesValue(const std::vector<ConfigObjectSummary>& list) {
    std::vector<json::Value> items;
    for (const ConfigObjectSummary& s : list) {
        items.push_back(json::Value::makeObject({{"id", json::Value::makeString(s.id)},
                                                 {"label", json::Value::makeString(s.label)},
                                                 {"show", json::Value::makeString(s.show)}}));
    }
    return json::Value::makeArray(std::move(items));
}

}  // namespace

bool parseConfigObjectsList(const std::string& body, std::vector<ConfigObjectSummary>* out) {
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.ok || parsed.value.type() != json::Type::kObject) return false;
    const json::Value* objects = member(parsed.value, "objects");
    if (objects == nullptr || objects->type() != json::Type::kArray) return false;
    out->clear();
    parseSummaries(*objects, out);
    return true;
}

bool isCommandTargetId(const std::string& id) {
    if (id.empty()) return false;
    for (const char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == ':' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string renderCommandTargetOptions(const std::vector<ConfigObjectSummary>* targets,
                                       const std::vector<ConfigObjectSummary>* shows, const std::string& noun) {
    if (targets == nullptr) {
        return placeholder("No " + noun + " found. Pair this FPP with ShowMesh on the ShowMesh plugin page.");
    }
    std::vector<std::pair<std::string, std::string>> entries;
    std::set<std::string> seen;
    for (const ConfigObjectSummary& target : *targets) {
        if (!isCommandTargetId(target.id) || !seen.insert(target.id).second) continue;
        std::string text = target.label.empty() ? target.id : target.label;
        if (shows != nullptr && !target.show.empty()) {
            for (const ConfigObjectSummary& show : *shows) {
                if (show.id != target.show) continue;
                text += " (" + (show.label.empty() ? show.id : show.label) + ")";
                break;
            }
        }
        entries.emplace_back(text, target.id);
    }
    std::sort(entries.begin(), entries.end());
    std::string body = "{";
    for (const auto& entry : entries) {
        const std::optional<std::string> text = jsonString(withoutTagOpeners(entry.first));
        if (!text) continue;
        if (body.size() > 1) body += ",";
        body += "\"" + entry.second + "\":" + *text;
    }
    if (body.size() == 1) return placeholder("No " + noun + " are set up in ShowMesh.");
    return body + "}";
}

CommandTargetLists::CommandTargetLists(std::string path) : path_(std::move(path)) {
    load();
    std::lock_guard<std::mutex> lock(mutex_);
    renderLocked();
}

void CommandTargetLists::load() {
    std::string text;
    if (path_.empty() || !readFileWhole(path_, &text)) return;
    const json::ParseResult parsed = json::parse(text);
    if (!parsed.ok || parsed.value.type() != json::Type::kObject) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::pair<const char*, List*> slots[] = {{"macros", &macros_}, {"actions", &actions_}, {"shows", &shows_}};
    for (const auto& slot : slots) {
        const json::Value* items = member(parsed.value, slot.first);
        if (items == nullptr || items->type() != json::Type::kArray) continue;
        std::vector<ConfigObjectSummary> list;
        parseSummaries(*items, &list);
        *slot.second = std::move(list);
    }
}

std::string CommandTargetLists::refresh(HttpTransport* transport, CredentialSource* credentials,
                                        const std::string& baseUrl, const std::function<bool()>& stopRequested) {
    std::string token;
    std::string error;
    if (transport == nullptr || credentials == nullptr || !credentials->token(&token, &error)) {
        return "no usable credential: " + error;
    }
    const std::pair<const char*, List*> kinds[] = {
        {"show.macro", &macros_}, {"show.action", &actions_}, {"show", &shows_}};
    std::string problems;
    bool changed = false;
    for (const auto& kind : kinds) {
        if (stopRequested && stopRequested()) break;
        HttpRequest request;
        request.url = joinUrlPath(baseUrl, std::string("/api/v1/config/") + kind.first);
        request.bearerToken = token;
        request.timeoutMillis = kListTimeoutMillis;
        request.maxResponseBytes = kListMaxResponseBytes;
        const HttpResponse response = transport->get(request);
        std::vector<ConfigObjectSummary> list;
        std::string problem;
        if (!response.transportOk) {
            problem = response.error.empty() ? "no response from the coordinator" : response.error;
        } else if (response.statusCode != 200) {
            problem = "the coordinator answered " + std::to_string(response.statusCode);
        } else if (!parseConfigObjectsList(response.body, &list)) {
            problem = "the coordinator's answer is not a list";
        }
        if (!problem.empty()) {
            problems += (problems.empty() ? "" : "; ") + std::string(kind.first) + ": " + problem;
            // An unreachable coordinator would only time out again on the next list.
            if (!response.transportOk) break;
            continue;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string before = serializeLocked();
        *kind.second = std::move(list);
        changed = changed || serializeLocked() != before;
    }
    if (changed) {
        std::string contents;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            renderLocked();
            contents = serializeLocked();
        }
        if (!path_.empty() && !contents.empty() && !writeFileAtomically(path_, contents)) {
            problems += (problems.empty() ? "" : "; ") + std::string("the lists could not be saved to ") + path_;
        }
    }
    return problems;
}

std::string CommandTargetLists::macrosBody() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return macrosBody_;
}

std::string CommandTargetLists::actionsBody() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return actionsBody_;
}

void CommandTargetLists::renderLocked() {
    const std::vector<ConfigObjectSummary>* shows = shows_ ? &*shows_ : nullptr;
    macrosBody_ = renderCommandTargetOptions(macros_ ? &*macros_ : nullptr, shows, "macros");
    actionsBody_ = renderCommandTargetOptions(actions_ ? &*actions_ : nullptr, shows, "actions");
}

std::string CommandTargetLists::serializeLocked() const {
    std::vector<json::Value::Member> members;
    if (macros_) members.emplace_back("macros", summariesValue(*macros_));
    if (actions_) members.emplace_back("actions", summariesValue(*actions_));
    if (shows_) members.emplace_back("shows", summariesValue(*shows_));
    const json::CanonicalResult rendered = json::canonicalize(json::Value::makeObject(std::move(members)));
    return rendered.ok ? rendered.text : std::string();
}

}  // namespace showmesh
