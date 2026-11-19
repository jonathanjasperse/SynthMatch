// synthmatch_edit: change a preset file with edits in the assistant's JSON
// format (what the language model writes), or see what the model is sent.
//
//   synthmatch_edit pad.json '{"edits":[{"param":"filter_cutoff_hz","op":"nudge","value":0.15}]}'
//   synthmatch_edit pad.json answer.txt            (the edits from a file)
//   synthmatch_edit pad.json --prompt "warmer"     (print what the language model would be sent)
//   synthmatch_edit pad.json --show                (describe the preset)

#include "synthmatch/Preset.h"
#include "synthmatch/assist/Assistant.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

void printUsage()
{
    std::puts(
        "usage: synthmatch_edit PRESET.json EDITS [options]\n"
        "       synthmatch_edit PRESET.json --prompt \"REQUEST\"\n"
        "\n"
        "Applies EDITS to a preset file. EDITS is JSON in the form the assistant's\n"
        "language model answers with, or a file holding it, e.g.\n"
        "  {\"edits\":[{\"param\":\"filter_cutoff_hz\",\"op\":\"nudge\",\"value\":0.15}]}\n"
        "(text around the JSON is ignored, so a model's whole answer works too).\n"
        "\n"
        "  --out PATH    write the result here instead of over PRESET.json\n"
        "  --dry-run     show the changes without writing anything\n"
        "  --show        describe the preset, and exit\n"
        "  --prompt      print the prompt the language model would be given for\n"
        "                REQUEST, and exit\n"
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
    std::string presetPath, input, outPath;
    bool dryRun = false, prompt = false, schema = false, show = false;

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
        } else if (arg == "--show") {
            show = true;
        } else if (arg == "--out") {
            if (++i >= argc)
                return fail("--out needs a path");
            outPath = argv[i];
        } else if (presetPath.empty()) {
            presetPath = arg;
        } else {
            input += (input.empty() ? "" : " ") + std::string(arg);
        }
    }
    if (schema) {
        std::puts(synthmatch::assist::modelReplySchema().dump(2).c_str());
        return 0;
    }
    if (presetPath.empty() || (input.empty() && !show)) {
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

    if (show) {
        std::puts(synthmatch::assist::describePreset(*preset).c_str());
        return 0;
    }
    if (prompt) {
        std::printf("--- system ---\n%s\n--- user ---\n%s\n", synthmatch::assist::modelSystemPrompt().c_str(),
                    synthmatch::assist::modelUserPrompt(input, preset->params).c_str());
        return 0;
    }

    // EDITS may also name a file holding them.
    if (input.find('{') == std::string::npos) {
        std::ifstream file(input, std::ios::binary);
        if (!file)
            return fail("EDITS is neither JSON nor a file that can be read: " + input);
        input.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    const auto reply = synthmatch::assist::parseModelReply(input, &error);
    if (!reply)
        return fail(error);
    const auto changes = synthmatch::assist::applyEdits(preset->params, reply->edits);
    if (!reply->text.empty())
        std::puts(reply->text.c_str());
    if (changes.empty()) {
        std::puts("Nothing changed (already at the limit).");
        return 0;
    }
    std::puts(synthmatch::assist::describeChanges(changes).c_str());

    if (dryRun)
        return 0;
    const std::string target = outPath.empty() ? presetPath : outPath;
    if (!synthmatch::savePreset(target, *preset, &error))
        return fail(error);
    std::printf("Wrote %s\n", target.c_str());
    return 0;
}
