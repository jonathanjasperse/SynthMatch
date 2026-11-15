#include "synthmatch/match/Optimizer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace synthmatch::match {

void symmetricEigen(const std::vector<double>& matrix, std::size_t n, std::vector<double>& vectors,
                    std::vector<double>& values)
{
    // Cyclic Jacobi: repeatedly rotate pairs of axes to zero the off-diagonal
    // entries. Simple, accurate, and fast enough for the sizes used here.
    std::vector<double> a = matrix;
    vectors.assign(n * n, 0.0);
    for (std::size_t i = 0; i < n; ++i)
        vectors[i * n + i] = 1.0;

    for (int sweep = 0; sweep < 60; ++sweep) {
        double off = 0.0;
        for (std::size_t p = 0; p < n; ++p)
            for (std::size_t q = p + 1; q < n; ++q)
                off += a[p * n + q] * a[p * n + q];
        if (off < 1.0e-22)
            break;

        for (std::size_t p = 0; p < n; ++p) {
            for (std::size_t q = p + 1; q < n; ++q) {
                const double apq = a[p * n + q];
                if (std::abs(apq) < 1.0e-300)
                    continue;
                const double theta = (a[q * n + q] - a[p * n + p]) / (2.0 * apq);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;

                for (std::size_t k = 0; k < n; ++k) {
                    const double akp = a[k * n + p], akq = a[k * n + q];
                    a[k * n + p] = c * akp - s * akq;
                    a[k * n + q] = s * akp + c * akq;
                }
                for (std::size_t k = 0; k < n; ++k) {
                    const double apk = a[p * n + k], aqk = a[q * n + k];
                    a[p * n + k] = c * apk - s * aqk;
                    a[q * n + k] = s * apk + c * aqk;
                }
                for (std::size_t k = 0; k < n; ++k) {
                    const double vkp = vectors[k * n + p], vkq = vectors[k * n + q];
                    vectors[k * n + p] = c * vkp - s * vkq;
                    vectors[k * n + q] = s * vkp + c * vkq;
                }
            }
        }
    }

    values.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        values[i] = a[i * n + i];
}

CmaEs::CmaEs(std::vector<double> mean, double sigma, std::size_t population, std::uint32_t seed)
    : n_(mean.size()), mean_(std::move(mean)), sigma_(sigma), rng_(seed * 2654435761u + 0x9E3779B9u)
{
    if (rng_ == 0)
        rng_ = 1;
    const double n = static_cast<double>(n_);

    lambda_ = population > 0 ? population : static_cast<std::size_t>(4.0 + std::floor(3.0 * std::log(n)));
    lambda_ = std::max<std::size_t>(lambda_, 4);
    mu_ = lambda_ / 2;

    // The better half of each generation pulls the cloud, best ones hardest.
    weights_.resize(mu_);
    double sum = 0.0;
    for (std::size_t i = 0; i < mu_; ++i) {
        weights_[i] = std::log(static_cast<double>(mu_) + 0.5) - std::log(static_cast<double>(i) + 1.0);
        sum += weights_[i];
    }
    double sumSquares = 0.0;
    for (auto& w : weights_) {
        w /= sum;
        sumSquares += w * w;
    }
    mueff_ = 1.0 / sumSquares;

    // Standard learning rates (Hansen, "The CMA Evolution Strategy: A Tutorial").
    cc_ = (4.0 + mueff_ / n) / (n + 4.0 + 2.0 * mueff_ / n);
    cs_ = (mueff_ + 2.0) / (n + mueff_ + 5.0);
    c1_ = 2.0 / ((n + 1.3) * (n + 1.3) + mueff_);
    cmu_ = std::min(1.0 - c1_, 2.0 * (mueff_ - 2.0 + 1.0 / mueff_) / ((n + 2.0) * (n + 2.0) + mueff_));
    damps_ = 1.0 + 2.0 * std::max(0.0, std::sqrt((mueff_ - 1.0) / (n + 1.0)) - 1.0) + cs_;
    chiN_ = std::sqrt(n) * (1.0 - 1.0 / (4.0 * n) + 1.0 / (21.0 * n * n));

    c_.assign(n_ * n_, 0.0);
    b_.assign(n_ * n_, 0.0);
    for (std::size_t i = 0; i < n_; ++i)
        c_[i * n_ + i] = b_[i * n_ + i] = 1.0;
    d_.assign(n_, 1.0);
    pc_.assign(n_, 0.0);
    ps_.assign(n_, 0.0);
    candidates_.assign(lambda_, std::vector<double>(n_));
    steps_.assign(lambda_, std::vector<double>(n_));
}

std::uint32_t CmaEs::nextRandom()
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

double CmaEs::gaussian()
{
    if (haveSpare_) {
        haveSpare_ = false;
        return spare_;
    }
    // Box-Muller.
    const double u1 = (static_cast<double>(nextRandom()) + 1.0) / 4294967297.0;
    const double u2 = static_cast<double>(nextRandom()) / 4294967296.0;
    const double radius = std::sqrt(-2.0 * std::log(u1));
    spare_ = radius * std::sin(2.0 * std::numbers::pi * u2);
    haveSpare_ = true;
    return radius * std::cos(2.0 * std::numbers::pi * u2);
}

const std::vector<std::vector<double>>& CmaEs::ask()
{
    std::vector<double> z(n_);
    for (std::size_t k = 0; k < lambda_; ++k) {
        for (auto& v : z)
            v = gaussian();
        // step = B * (D .* z): a sample from the learned shape of the cloud.
        for (std::size_t i = 0; i < n_; ++i) {
            double y = 0.0;
            for (std::size_t j = 0; j < n_; ++j)
                y += b_[i * n_ + j] * d_[j] * z[j];
            steps_[k][i] = y;
            candidates_[k][i] = mean_[i] + sigma_ * y;
        }
    }
    return candidates_;
}

void CmaEs::tell(const std::vector<double>& fitness)
{
    std::vector<std::size_t> order(lambda_);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return fitness[a] < fitness[b]; });

    // Move the mean towards the best candidates.
    std::vector<double> step(n_, 0.0);
    for (std::size_t i = 0; i < mu_; ++i)
        for (std::size_t j = 0; j < n_; ++j)
            step[j] += weights_[i] * steps_[order[i]][j];
    for (std::size_t j = 0; j < n_; ++j)
        mean_[j] += sigma_ * step[j];

    // Step-size path: C^(-1/2) * step = B * (B^T step ./ D).
    std::vector<double> projected(n_, 0.0), whitened(n_, 0.0);
    for (std::size_t j = 0; j < n_; ++j) {
        double v = 0.0;
        for (std::size_t i = 0; i < n_; ++i)
            v += b_[i * n_ + j] * step[i];
        projected[j] = v / d_[j];
    }
    for (std::size_t i = 0; i < n_; ++i)
        for (std::size_t j = 0; j < n_; ++j)
            whitened[i] += b_[i * n_ + j] * projected[j];

    const double csFactor = std::sqrt(cs_ * (2.0 - cs_) * mueff_);
    double psNorm = 0.0;
    for (std::size_t i = 0; i < n_; ++i) {
        ps_[i] = (1.0 - cs_) * ps_[i] + csFactor * whitened[i];
        psNorm += ps_[i] * ps_[i];
    }
    psNorm = std::sqrt(psNorm);

    ++generation_;
    const double n = static_cast<double>(n_);
    const double expected = std::sqrt(1.0 - std::pow(1.0 - cs_, 2.0 * static_cast<double>(generation_)));
    const bool hsig = psNorm / expected / chiN_ < 1.4 + 2.0 / (n + 1.0);

    const double ccFactor = hsig ? std::sqrt(cc_ * (2.0 - cc_) * mueff_) : 0.0;
    for (std::size_t i = 0; i < n_; ++i)
        pc_[i] = (1.0 - cc_) * pc_[i] + ccFactor * step[i];

    // Covariance update: remember the successful direction (rank one) and the
    // spread of the best candidates (rank mu).
    const double keep = 1.0 - c1_ - cmu_ + (hsig ? 0.0 : c1_ * cc_ * (2.0 - cc_));
    for (std::size_t i = 0; i < n_; ++i) {
        for (std::size_t j = 0; j <= i; ++j) {
            double rankMu = 0.0;
            for (std::size_t k = 0; k < mu_; ++k)
                rankMu += weights_[k] * steps_[order[k]][i] * steps_[order[k]][j];
            const double value = keep * c_[i * n_ + j] + c1_ * pc_[i] * pc_[j] + cmu_ * rankMu;
            c_[i * n_ + j] = c_[j * n_ + i] = value;
        }
    }

    sigma_ *= std::exp((cs_ / damps_) * (psNorm / chiN_ - 1.0));
    sigma_ = std::clamp(sigma_, 1.0e-8, 10.0);

    // Recomputing the eigenvectors every generation would be wasteful.
    const double interval = 1.0 / ((c1_ + cmu_) * n * 10.0);
    if (static_cast<double>(generation_ - eigenGeneration_) >= interval) {
        eigenGeneration_ = generation_;
        updateEigen();
    }
}

void CmaEs::updateEigen()
{
    std::vector<double> values;
    symmetricEigen(c_, n_, b_, values);
    for (std::size_t i = 0; i < n_; ++i)
        d_[i] = std::sqrt(std::max(values[i], 1.0e-20));
}

} // namespace synthmatch::match
