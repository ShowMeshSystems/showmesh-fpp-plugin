#pragma once

#include <memory>
#include <string>
#include <vector>

// Stub. See ../../README.md. Global scope (no namespace) on purpose: the
// real header declares Command/CommandArg/CommandManager unqualified, and
// the adapters reference them unqualified.

class CommandArg {
 public:
    CommandArg(const std::string& name, const std::string& type, const std::string& description);
    CommandArg& setRange(int minValue, int maxValue);
    CommandArg& setDefaultValue(const std::string& value);
};

class Command {
 public:
    class Result {
     public:
        explicit Result(const std::string& message);
        virtual ~Result() = default;
    };
    class ErrorResult : public Result {
     public:
        explicit ErrorResult(const std::string& message);
    };

    Command(const std::string& name, const std::string& description);
    virtual ~Command() = default;
    virtual std::unique_ptr<Result> run(const std::vector<std::string>& args) = 0;

    std::vector<CommandArg> args;
};

class CommandManager {
 public:
    static CommandManager INSTANCE;
    void addCommand(Command* command);
    void removeCommand(Command* command);
    void removeCommand(const std::string& name);
};
