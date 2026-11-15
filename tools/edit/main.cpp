// synthmatch_edit: change a preset file by describing the change in words.
//
//   synthmatch_edit pad.json "make it brighter and add some reverb"
//   synthmatch_edit pad.json --prompt "warmer"     (print what a language model would be sent)

#include "synthmatch/Preset.h"
#include "synthmatch/assist/Assistant.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

void printUsage()
{
    std::puts(
        "usage: synthmatch_edit PRESET.json \"REQUEST\" [options]\n"
        "\n"
        "Applies a request such as \"make it brighter\" to a preset file, using the\n"
        "built-in rules (the plugin can also use a local language model).\n"
        "\n"
        "  --out PATH    write the result here instead of over PRESET.json\n"
        "  --dry-run     show the changes without writing anything\n"
        "  --prompt      print the prompt a language model would be given, and exit\n"
        "  --schema      print the JSON Schema its answer must follow, and exit\n"
        "  --help        show this message");
}

int fail(const std::string& message)
{
    std::fprintf(stderr, "error: %s\n", message.c_str());
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    std::string presetPath, request, outPath;
    bool dryRun = false, prompt = false, schema = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
        if (arg == "--dry-run") {
            dryRun = true;
        } else if (arg == "--prompt") {
            prompt = true;
        } else if (arg == "--schema") {
            schema = true;
        } else if (arg == "--out") {
            if (++i >= argc)
                return fail("--out needs a path");
            outPath = argv[i];
        } else if (presetPath.empty()) {
            presetPath = arg;
        } else {
            request += (request.empty() ? "" : " ") + std::string(arg);
        }
    }
    if (schema) {
        std::puts(synthmatch::assist::modelReplySchema().dump(2).c_str());
        return 0;
    }
    if (presetPath.empty() || request.empty()) {
        printUsage();
        return 2;
    }

    std::string error;
    std::vector<std::string> warnings;
    auto preset = synthmatch::loadPreset(presetPath, &error, &warnings);
    if (!preset)
        return fail(error);
    for (const auto& w : warnings)
        std::fprintf(stderr, "warning: %s\n", w.c_str());

    if (prompt) {
        std::printf("--- system ---\n%s\n--- user ---\n%s\n", synthmatch::assist::modelSystemPrompt().c_str(),
                    synthmatch::assist::modelUserPrompt(request, preset->params).c_str());
        return 0;
    }

    using synthmatch::assist::Reply;
    const Reply reply = synthmatch::assist::interpret(request, preset->params);
    switch (reply.action) {
    case Reply::Action::Help:
        std::puts(synthmatch::assist::helpText().c_str());
        return 0;
    case Reply::Action::Show:
        std::puts(synthmatch::assist::describePreset(*preset).c_str());
        return 0;
    case Reply::Action::Edit:
        break;
    case Reply::Action::Reset:
        preset->params = synthmatch::defaultValues();
        preset->matchEq.fill(0.0f);
        std::puts("Reset to the initial preset.");
        break;
    default:
        return fail(reply.text.empty() ? "that only works in the plugin" : reply.text);
    }

    if (reply.action == Reply::Action::Edit) {
        const auto changes = synthmatch::assist::applyEdits(preset->params, reply.edits);
        if (!reply.text.empty())
            std::puts(reply.text.c_str());
        if (changes.empty()) {
            std::puts("Nothing changed (already at the limit).");
            return 0;
        }
        std::puts(synthmatch::assist::describeChanges(changes).c_str());
    }

    if (dryRun)
        return 0;
    const std::string target = outPath.empty() ? presetPath : outPath;
    if (!synthmatch::savePreset(target, *preset, &error))
        return fail(error);
    std::printf("Wrote %s\n", target.c_str());
    return 0;
}
