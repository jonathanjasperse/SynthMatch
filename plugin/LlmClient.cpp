#include "LlmClient.h"

#include <optional>

namespace llm {

namespace {

using synthmatch::Json;

juce::String trimSlash(juce::String s)
{
    s = s.trim();
    while (s.endsWithChar('/'))
        s = s.dropLastCharacters(1);
    return s;
}

std::unique_ptr<juce::InputStream> open(const juce::String& url, const juce::String& postBody, int timeoutMs, int& status)
{
    juce::URL target(url);
    auto handling = juce::URL::ParameterHandling::inAddress;
    if (postBody.isNotEmpty()) {
        target = target.withPOSTData(postBody);
        handling = juce::URL::ParameterHandling::inPostData;
    }
    status = 0;
    return target.createInputStream(juce::URL::InputStreamOptions(handling)
                                        .withExtraHeaders("Content-Type: application/json")
                                        .withConnectionTimeoutMs(timeoutMs)
                                        .withStatusCode(&status));
}

std::optional<Json> getJson(const juce::String& url, int& status)
{
    auto stream = open(url, {}, 1500, status);
    if (!stream)
        return std::nullopt;
    const auto text = stream->readEntireStreamAsString();
    return Json::parse(text.toStdString());
}

bool modelMatches(const juce::String& installed, const juce::String& wanted)
{
    if (installed == wanted || installed == wanted + ":latest")
        return true;
    // "qwen2.5:1.5b" also accepts "qwen2.5:1.5b-instruct-q4_K_M".
    return wanted.containsChar(':') && installed.startsWith(wanted);
}

Json message(const char* role, const std::string& content)
{
    Json m = Json::object();
    m.set("role", Json(role));
    m.set("content", Json(content));
    return m;
}

} // namespace

juce::StringArray Probe::names() const
{
    juce::StringArray out;
    for (const auto& m : models)
        out.add(m.name);
    return out;
}

const ModelInfo* Probe::find(const juce::String& model) const
{
    for (const auto& m : models)
        if (modelMatches(m.name, model))
            return &m;
    return nullptr;
}

juce::String Probe::smallest(double maxBytes) const
{
    const ModelInfo* best = nullptr;
    for (const auto& m : models)
        if (m.bytes > 0.0 && m.bytes <= maxBytes && (best == nullptr || m.bytes < best->bytes))
            best = &m;
    return best ? best->name : juce::String();
}

Probe probe(const Settings& settings)
{
    Probe result;
    const auto server = trimSlash(settings.server);
    int status = 0;

    // Ollama lists its models at /api/tags.
    if (const auto tags = getJson(server + "/api/tags", status); tags && status == 200) {
        result.reachable = result.ollama = true;
        if (const Json* models = tags->find("models"); models && models->isArray()) {
            for (const auto& m : models->items()) {
                ModelInfo info;
                if (const Json* name = m.find("name"); name && name->isString())
                    info.name = juce::String(name->string());
                if (const Json* size = m.find("size"); size && size->isNumber())
                    info.bytes = size->number();
                if (const Json* caps = m.find("capabilities"); caps && caps->isArray())
                    for (const auto& c : caps->items())
                        info.thinking = info.thinking || (c.isString() && c.string() == "thinking");
                if (info.name.isNotEmpty())
                    result.models.push_back(info);
            }
        }
    } else if (const auto list = getJson(server + "/v1/models", status); list && status == 200) {
        // The OpenAI-style API lists them at /v1/models.
        result.reachable = true;
        if (const Json* data = list->find("data"); data && data->isArray())
            for (const auto& m : data->items())
                if (const Json* id = m.find("id"); id && id->isString())
                    result.models.push_back({juce::String(id->string()), 0.0, false});
    } else {
        result.error = "No model server at " + server;
        return result;
    }

    result.hasModel = result.find(settings.model) != nullptr;
    if (!result.ollama && result.models.empty())
        result.hasModel = true; // some servers serve one model and don't list it
    if (!result.hasModel)
        result.error = "The model " + settings.model + " isn't installed";
    return result;
}

juce::String pull(const Settings& settings, const juce::String& model, const std::atomic<bool>& cancel,
                  const std::function<void(double, double)>& onProgress)
{
    Json body = Json::object();
    body.set("model", Json(model.toStdString()));
    body.set("stream", Json(true));

    int status = 0;
    auto stream = open(trimSlash(settings.server) + "/api/pull", juce::String(body.dump(0)), 30000, status);
    if (!stream)
        return "Couldn't reach Ollama at " + trimSlash(settings.server);
    if (status != 200)
        return "Ollama answered with an error (HTTP " + juce::String(status) + "): " +
               stream->readEntireStreamAsString().substring(0, 200);

    // One JSON object per line: {"status": "pulling ...", "completed": n, "total": n}, then {"status": "success"}.
    // A model is several files; progress is reported for the largest one.
    double largest = 0.0;
    while (!stream->isExhausted()) {
        if (cancel.load())
            return "Cancelled";
        const auto line = stream->readNextLine().trim();
        if (line.isEmpty())
            continue;
        const auto chunk = Json::parse(line.toStdString());
        if (!chunk)
            continue;
        if (const Json* error = chunk->find("error"))
            return juce::String(error->isString() ? error->string() : error->dump(0));
        const Json* total = chunk->find("total");
        const Json* done = chunk->find("completed");
        if (total && total->isNumber() && total->number() >= largest) {
            largest = total->number();
            if (onProgress)
                onProgress(done && done->isNumber() ? done->number() : 0.0, largest);
        }
        if (const Json* s = chunk->find("status"); s && s->isString() && s->string() == "success")
            return {};
    }
    return "The download stopped before it finished";
}

juce::File installedApp()
{
#if JUCE_WINDOWS
    return juce::File::getSpecialLocation(juce::File::windowsLocalAppData).getChildFile("Programs/Ollama/ollama app.exe");
#elif JUCE_MAC
    return juce::File("/Applications/Ollama.app");
#else
    return {};
#endif
}

Answer chat(const Settings& settings, bool ollama, bool noThinking, const std::string& system, const std::string& user,
            const Json& schema, const std::atomic<bool>& cancel, const std::function<void(int)>& onProgress)
{
    Answer answer;
    const auto server = trimSlash(settings.server);

    Json messages = Json::array();
    messages.push(message("system", system));
    messages.push(message("user", user));

    Json body = Json::object();
    body.set("model", Json(settings.model.toStdString()));
    body.set("messages", std::move(messages));
    body.set("stream", Json(true));

    juce::String url;
    if (ollama) {
        url = server + "/api/chat";
        body.set("format", schema);
        Json options = Json::object();
        options.set("temperature", Json(0.1));
        options.set("num_predict", Json(300));
        body.set("options", std::move(options));
        body.set("keep_alive", Json("30m")); // keep the model loaded between requests
        if (noThinking)
            body.set("think", Json(false)); // a reasoning model would spend seconds thinking first
    } else {
        url = server + "/v1/chat/completions";
        body.set("temperature", Json(0.1));
        body.set("max_tokens", Json(300));
        Json format = Json::object();
        format.set("type", Json("json_schema"));
        Json named = Json::object();
        named.set("name", Json("synth_edits"));
        named.set("schema", schema);
        format.set("json_schema", std::move(named));
        body.set("response_format", std::move(format));
    }

    int status = 0;
    auto stream = open(url, juce::String(body.dump(0)), 30000, status);
    if (!stream) {
        answer.error = "Couldn't reach the model server at " + server;
        return answer;
    }
    if (status != 200) {
        answer.error = "The model server answered with an error (HTTP " + juce::String(status) + "): " +
                       stream->readEntireStreamAsString().substring(0, 200);
        return answer;
    }

    // The reply arrives a few characters at a time: one JSON object per line
    // (Ollama), or "data: {...}" lines (the OpenAI style).
    std::string text;
    while (!stream->isExhausted() && !cancel.load()) {
        auto line = stream->readNextLine().trim();
        if (line.isEmpty())
            continue;
        if (!ollama) {
            if (!line.startsWith("data:"))
                continue;
            line = line.substring(5).trim();
            if (line == "[DONE]")
                break;
        }
        const auto chunk = Json::parse(line.toStdString());
        if (!chunk)
            continue;
        if (const Json* error = chunk->find("error")) {
            answer.error = "The model server reported: " +
                           juce::String(error->isString() ? error->string() : error->dump(0));
            return answer;
        }
        if (ollama) {
            if (const Json* m = chunk->find("message"))
                if (const Json* content = m->find("content"); content && content->isString())
                    text += content->string();
            if (const Json* done = chunk->find("done"); done && done->isBool() && done->boolean())
                break;
        } else if (const Json* choices = chunk->find("choices"); choices && choices->isArray() && !choices->items().empty()) {
            if (const Json* delta = choices->items()[0].find("delta"))
                if (const Json* content = delta->find("content"); content && content->isString())
                    text += content->string();
        }
        if (onProgress)
            onProgress(static_cast<int>(text.size()));
    }

    if (cancel.load()) {
        answer.error = "Cancelled";
        return answer;
    }
    answer.ok = !text.empty();
    answer.text = juce::String::fromUTF8(text.c_str());
    if (!answer.ok)
        answer.error = "The model gave an empty answer";
    return answer;
}

} // namespace llm
