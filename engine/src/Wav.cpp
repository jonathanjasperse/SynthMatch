#include "synthmatch/Render.h"

#include <cstdint>
#include <cstring>
#include <fstream>

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

} // namespace synthmatch
