#pragma once

// CMA-ES: a search method for problems where all you can do is try a setting
// and see how good it is. It keeps a cloud of candidate settings, moves the
// cloud towards the better ones, and learns which directions are worth
// exploring (the "covariance matrix"). No gradients needed, which suits a
// synth: we just render each candidate and compare it with the reference.
//
// Usage: ask() for a batch of candidates, score them (lower is better), tell().

#include <cstddef>
#include <cstdint>
#include <vector>

namespace synthmatch::match {

class CmaEs {
public:
    // mean: the starting point. sigma: the initial step size, in the same units.
    // population 0 picks the usual default for the number of dimensions.
    CmaEs(std::vector<double> mean, double sigma, std::size_t population = 0, std::uint32_t seed = 1);

    const std::vector<std::vector<double>>& ask();
    void tell(const std::vector<double>& fitness);

    std::size_t dimensions() const { return n_; }
    std::size_t population() const { return lambda_; }
    const std::vector<double>& mean() const { return mean_; }
    double sigma() const { return sigma_; }

private:
    void updateEigen();
    double gaussian();
    std::uint32_t nextRandom();

    std::size_t n_, lambda_, mu_;
    std::vector<double> weights_;
    double mueff_ = 0, cc_ = 0, cs_ = 0, c1_ = 0, cmu_ = 0, damps_ = 0, chiN_ = 0;

    std::vector<double> mean_;
    double sigma_;
    std::vector<double> c_;        // covariance, n x n
    std::vector<double> b_;        // its eigenvectors (columns), n x n
    std::vector<double> d_;        // square roots of its eigenvalues
    std::vector<double> pc_, ps_;  // evolution paths
    std::vector<std::vector<double>> candidates_, steps_;
    std::size_t generation_ = 0, eigenGeneration_ = 0;

    std::uint32_t rng_;
    bool haveSpare_ = false;
    double spare_ = 0.0;
};

// Eigen-decomposition of a symmetric n x n matrix (row-major). On return
// `vectors` holds the eigenvectors as columns and `values` the eigenvalues.
void symmetricEigen(const std::vector<double>& matrix, std::size_t n, std::vector<double>& vectors,
                    std::vector<double>& values);

} // namespace synthmatch::match
