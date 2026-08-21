#pragma once

#include <memory>
#include <string>
#include <vector>

#include "commands/Commands.h"
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
        return std::make_unique<Command::Result>(outcome.message);
    }

 private:
    ShowMeshRuntime* runtime_;
};

}  // namespace adapter
}  // namespace showmesh
