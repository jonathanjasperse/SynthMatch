#pragma once

// A small radix-2 FFT. Sizes must be powers of two.

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <vector>

namespace synthmatch::match {

class Fft {
public:
    explicit Fft(std::size_t size) : size_(size), reversed_(size), twiddles_(size / 2)
    {
        std::size_t bits = 0;
        while ((std::size_t{1} << bits) < size)
            ++bits;
        for (std::size_t i = 0; i < size; ++i) {
            std::size_t r = 0;
            for (std::size_t b = 0; b < bits; ++b)
                if (i & (std::size_t{1} << b))
                    r |= std::size_t{1} << (bits - 1 - b);
            reversed_[i] = r;
        }
        for (std::size_t k = 0; k < size / 2; ++k) {
            const double angle = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(size);
            twiddles_[k] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
        }
    }

    std::size_t size() const { return size_; }

    // In-place forward transform of size() complex values.
    void forward(std::complex<float>* data) const
    {
        for (std::size_t i = 0; i < size_; ++i)
            if (i < reversed_[i])
                std::swap(data[i], data[reversed_[i]]);

        for (std::size_t half = 1; half < size_; half <<= 1) {
            const std::size_t step = size_ / (half << 1);
            for (std::size_t start = 0; start < size_; start += half << 1) {
                for (std::size_t k = 0; k < half; ++k) {
                    const std::complex<float> w = twiddles_[k * step];
                    const std::complex<float> a = data[start + k];
                    const std::complex<float> b = data[start + k + half];
                    // Written out by hand: std::complex multiplication checks for NaNs.
                    const std::complex<float> t(b.real() * w.real() - b.imag() * w.imag(),
                                                b.real() * w.imag() + b.imag() * w.real());
                    data[start + k] = a + t;
                    data[start + k + half] = a - t;
                }
            }
        }
    }

private:
    std::size_t size_;
    std::vector<std::size_t> reversed_;
    std::vector<std::complex<float>> twiddles_;
};

} // namespace synthmatch::match
