#include "synthmatch/Render.h"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iterator>

namespace synthmatch {

namespace {

void put16(std::vector<unsigned char>& b, std::uint16_t v)
{
    b.push_back(static_cast<unsigned char>(v & 0xff));
    b.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
}

void put32(std::vector<unsigned char>& b, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xff));
}

void putTag(std::vector<unsigned char>& b, const char* tag) { b.insert(b.end(), tag, tag + 4); }

} // namespace

bool writeWav(const std::filesystem::path& path, const StereoBuffer& audio, std::string* error)
{
    auto fail = [error](const std::string& message) {
        if (error)
            *error = message;
        return false;
    };

    if (audio.left.size() != audio.right.size())
        return fail("left and right channels have different lengths");

    constexpr std::uint16_t channels = 2;
    constexpr std::uint16_t bitsPerSample = 32;
    constexpr std::uint16_t blockAlign = channels * bitsPerSample / 8;
    constexpr std::uint16_t formatIeeeFloat = 3;

    const auto frames = static_cast<std::uint64_t>(audio.numFrames());
    const auto dataBytes = frames * blockAlign;
    if (dataBytes > 0xFFFFFF00ull)
        return fail("audio too long for a WAV file");

    const auto sampleRate = static_cast<std::uint32_t>(audio.sampleRate + 0.5);

    std::vector<unsigned char> b;
    b.reserve(58 + static_cast<std::size_t>(dataBytes));

    putTag(b, "RIFF");
    put32(b, static_cast<std::uint32_t>(4 + (8 + 18) + (8 + 4) + (8 + dataBytes)));
    putTag(b, "WAVE");

    putTag(b, "fmt ");
    put32(b, 18);
    put16(b, formatIeeeFloat);
    put16(b, channels);
    put32(b, sampleRate);
    put32(b, sampleRate * blockAlign);
    put16(b, blockAlign);
    put16(b, bitsPerSample);
    put16(b, 0); // cbSize

    putTag(b, "fact"); // required for non-PCM formats
    put32(b, 4);
    put32(b, static_cast<std::uint32_t>(frames));

    putTag(b, "data");
    put32(b, static_cast<std::uint32_t>(dataBytes));

    for (std::size_t i = 0; i < audio.numFrames(); ++i) {
        for (const float sample : {audio.left[i], audio.right[i]}) {
            std::uint32_t bits;
            std::memcpy(&bits, &sample, sizeof bits);
            put32(b, bits);
        }
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return fail("cannot open " + path.string() + " for writing");
    file.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!file)
        return fail("failed writing " + path.string());
    return true;
}

namespace {

std::uint32_t get32(const std::vector<unsigned char>& b, std::size_t at)
{
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::uint16_t get16(const std::vector<unsigned char>& b, std::size_t at)
{
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

bool tagIs(const std::vector<unsigned char>& b, std::size_t at, const char* tag)
{
    return std::memcmp(b.data() + at, tag, 4) == 0;
}

} // namespace

std::optional<StereoBuffer> readWav(const std::filesystem::path& path, std::string* error)
{
    auto fail = [error](const std::string& message) -> std::optional<StereoBuffer> {
        if (error)
            *error = message;
        return std::nullopt;
    };

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return fail("cannot open " + path.string());
    const std::vector<unsigned char> b((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    if (b.size() < 12 || !tagIs(b, 0, "RIFF") || !tagIs(b, 8, "WAVE"))
        return fail(path.string() + " is not a WAV file");

    constexpr std::uint16_t formatPcm = 1, formatFloat = 3, formatExtensible = 0xFFFE;
    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t sampleRate = 0;
    std::size_t dataAt = 0, dataBytes = 0;

    // Walk the chunks. Each is a 4-byte tag, a 4-byte size, then the data,
    // padded to an even length.
    for (std::size_t at = 12; at + 8 <= b.size();) {
        const std::size_t size = get32(b, at + 4);
        const std::size_t body = at + 8;
        if (tagIs(b, at, "fmt ") && body + 16 <= b.size()) {
            format = get16(b, body);
            channels = get16(b, body + 2);
            sampleRate = get32(b, body + 4);
            bits = get16(b, body + 14);
            if (format == formatExtensible && size >= 26 && body + 26 <= b.size())
                format = get16(b, body + 24); // the real format is in the extension
        } else if (tagIs(b, at, "data")) {
            dataAt = body;
            dataBytes = std::min(size, b.size() - body);
            break;
        }
        at = body + size + (size & 1);
    }

    if (channels == 0 || sampleRate == 0 || dataAt == 0)
        return fail(path.string() + " has no audio data");

    const bool isFloat = format == formatFloat && (bits == 32 || bits == 64);
    const bool isPcm = format == formatPcm && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    if (!isFloat && !isPcm)
        return fail(path.string() + " uses an unsupported WAV encoding");

    const std::size_t bytesPerSample = bits / 8;
    const std::size_t frames = dataBytes / (bytesPerSample * channels);

    const auto sampleAt = [&](std::size_t frame, std::size_t channel) -> float {
        const std::size_t at = dataAt + (frame * channels + channel) * bytesPerSample;
        if (isFloat) {
            if (bits == 32) {
                float v;
                std::memcpy(&v, b.data() + at, sizeof v);
                return v;
            }
            double v;
            std::memcpy(&v, b.data() + at, sizeof v);
            return static_cast<float>(v);
        }
        switch (bits) {
        case 8:
            return (static_cast<float>(b[at]) - 128.0f) / 128.0f;
        case 16:
            return static_cast<float>(static_cast<std::int16_t>(get16(b, at))) / 32768.0f;
        case 24: {
            std::int32_t v = b[at] | (b[at + 1] << 8) | (b[at + 2] << 16);
            if (v & 0x800000)
                v |= ~0xFFFFFF; // sign-extend
            return static_cast<float>(v) / 8388608.0f;
        }
        default:
            return static_cast<float>(static_cast<double>(static_cast<std::int32_t>(get32(b, at))) / 2147483648.0);
        }
    };

    StereoBuffer audio;
    audio.sampleRate = sampleRate;
    audio.left.resize(frames);
    audio.right.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        audio.left[i] = sampleAt(i, 0);
        audio.right[i] = channels > 1 ? sampleAt(i, 1) : audio.left[i];
    }
    return audio;
}

} // namespace synthmatch
