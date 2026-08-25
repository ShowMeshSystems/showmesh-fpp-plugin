#pragma once

#include <memory>
#include <string>
#include <vector>

#include "commands/Commands.h"
#include "log.h"
#include "showmesh/runtime.h"

// The registered FPP Action. Its shape is identical on both majors, so it
// lives here and is compiled separately into each adapter rather than
// being duplicated. It is a header on purpose: each adapter's object
// carries its own vtable, which is what FPP 10's plugin-unload contract
// requires of a Command a plugin registers.

namespace showmesh {
namespace adapter {

class SetBrightnessCeilingCommand : public Command {
 public:
    explicit SetBrightnessCeilingCommand(ShowMeshRuntime* runtime)
        : Command(kBrightnessCommandName,
                  "Set the ShowMesh brightness ceiling, optionally fading to it over a number of seconds"),
          runtime_(runtime) {
        args.push_back(CommandArg(kTargetPercentArgument, "int", "Target brightness ceiling percent").setRange(0, 100));
        args.push_back(CommandArg(kFadeSecondsArgument, "int", "Fade duration in seconds, 0 to apply immediately")
                           .setRange(0, static_cast<int>(kMaxFadeSeconds))
                           .setDefaultValue("0"));
    }

    std::unique_ptr<Command::Result> run(const std::vector<std::string>& commandArgs) override {
        if (runtime_ == nullptr) {
            return std::make_unique<Command::ErrorResult>("the ShowMesh plugin is not running");
        }
        if (commandArgs.empty()) {
            return std::make_unique<Command::ErrorResult>(
                "a target brightness ceiling percent between 0 and 100 is required");
        }
        // The fade argument is optional, and an absent one means apply now
        // rather than an error: an operator setting a ceiling with no fade
        // is the ordinary case.
        const std::string fadeSeconds = commandArgs.size() > 1 ? commandArgs[1] : std::string("0");
        const CommandOutcome outcome = runtime_->applyBrightnessCommand(commandArgs[0], fadeSeconds);
        if (!outcome.ok) {
            return std::make_unique<Command::ErrorResult>(outcome.message);
        }
        // Persisted here, on the operator action that changed the target,
        // rather than only at teardown. This is the path that has to work
        // on FPP 9, which has no unload or shutdown hook at all: its only
        // flush lived in the destructor, and an fppd ended by a signal
        // never runs it, so setting a ceiling and stopping fppd gracefully
        // was observed to persist nothing and leave the darker-safe
        // restart guarantee not holding on the version the deployed fleet
        // runs.
        //
        // It also cannot be done from the channel-data path alone, which
        // was tried first: modifyChannelData only runs while fppd is
        // actually outputting, so a ceiling set on an idle host persisted
        // nothing. That path still persists a target adopted from
        // MultiSync, which never reaches this command.
        //
        // The result is not failed on a failed write. The ceiling is
        // already applied and live at this point, and reporting the
        // operator's command as failed because a file write failed would
        // misdescribe what happened to the wall. It is still logged: a
        // persistently failing flush is otherwise invisible, and it is
        // also what would leave the darker-safe restart guarantee unable
        // to hold.
        if (!runtime_->flushBrightnessState()) {
            LogErr(VB_PLUGIN,
                   "ShowMesh brightness state flush failed after a command; the darker-safe restart guarantee "
                   "may not hold\n");
        }
        return std::make_unique<Command::Result>(outcome.message);
    }

 private:
    ShowMeshRuntime* runtime_;
};

}  // namespace adapter
}  // namespace showmesh
