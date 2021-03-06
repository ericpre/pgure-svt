/**********************
# Author: Tom Furnival
# License: GPLv3
***********************/

#ifndef SVT_HPP
#define SVT_HPP

#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <vector>
#include <armadillo>

#include "utils.hpp"

namespace pguresvt
{
  template <typename T>
  class SVT
  {
  public:
    SVT(const arma::icube &patches,
        const uint32_t Nx,
        const uint32_t Ny,
        const uint32_t Nt,
        const uint32_t blockSize,
        const uint32_t blockOverlap,
        const bool expWeighting) : patches(patches),
                                   Nx(Nx), Ny(Ny), Nt(Nt),
                                   blockSize(blockSize),
                                   blockOverlap(blockOverlap),
                                   expWeighting(expWeighting)
    {
      nxMbs = Nx - blockSize;
      nyMbs = Ny - blockSize;
      nxMbsDbo = nxMbs / blockOverlap;
      nyMbsDbo = nyMbs / blockOverlap;
      vecSize = (1 + nxMbsDbo) * (1 + nyMbsDbo);

      block.set_size(blockSize * blockSize, Nt);
      Ublock.set_size(blockSize * blockSize, Nt);
      Vblock.set_size(Nt, Nt);
      Sblock.set_size(Nt);
      Sthresh.set_size(Nt);
    };

    ~SVT()
    {
      U.clear();
      S.clear();
      V.clear();
    };

    // Perform SVD on each block in the image sequence,
    // subject to the block overlap restriction
    void Decompose(const arma::Cube<T> &u)
    {
      // Fix block overlap parameter
      arma::uvec firstPatches(vecSize);
      uint32_t kiter = 0;
      for (size_t i = 0; i < 1 + nyMbs; i += blockOverlap)
      {
        for (size_t j = 0; j < 1 + nxMbs; j += blockOverlap)
        {
          firstPatches(kiter) = i * nyMbs + j;
          kiter++;
        }
      }

      // Code must include right and bottom edges
      // of the image sequence to ensure an
      // accurate PGURE reconstruction
      arma::uvec patchesBottomEdge(1 + nyMbsDbo);
      for (size_t i = 0; i < 1 + nyMbs; i += blockOverlap)
      {
        patchesBottomEdge(i / blockOverlap) = (nyMbs + 1) * i + nxMbs;
      }

      arma::uvec patchesRightEdge(1 + nxMbsDbo);
      for (size_t i = 0; i < 1 + nxMbs; i += blockOverlap)
      {
        patchesRightEdge(i / blockOverlap) = (nyMbs + 1) * nxMbs + i;
      }

      // Concatenate and find unique indices
      arma::uvec joinPatches(vecSize + 1 + nyMbsDbo + 1 + nxMbsDbo);
      joinPatches(arma::span(0, vecSize - 1)) = firstPatches;
      joinPatches(arma::span(vecSize, vecSize + nyMbsDbo)) = patchesRightEdge;
      joinPatches(arma::span(vecSize + nyMbsDbo + 1, vecSize + nyMbsDbo + 1 + nxMbsDbo)) = patchesBottomEdge;
      actualPatches = arma::sort(joinPatches.elem(arma::find_unique(joinPatches)));

      newVecSize = actualPatches.n_elem; // Get new vector size
      U.resize(newVecSize);              // Memory allocation
      S.resize(newVecSize);
      V.resize(newVecSize);

      for (size_t it = 0; it < newVecSize; it++)
      {
        for (size_t k = 0; k < Nt; k++) // Extract block
        {
          int newY = patches(0, actualPatches(it), k);
          int newX = patches(1, actualPatches(it), k);
          block.col(k) = arma::vectorise(
              u(arma::span(newY, newY + blockSize - 1),
                arma::span(newX, newX + blockSize - 1),
                arma::span(k)));
        }

        arma::svd_econ(Ublock, Sblock, Vblock, block);

        U[it] = Ublock;
        S[it] = Sblock;
        V[it] = Vblock;
      }
      return;
    };

    // Reconstruct block in the image sequence after thresholding
    arma::Cube<T> Reconstruct(const double lambda)
    {
      arma::Cube<T> v = arma::zeros<arma::Cube<T>>(Nx, Ny, Nt);
      arma::Cube<T> weights = arma::zeros<arma::Cube<T>>(Nx, Ny, Nt);
      arma::Mat<T> weightsInc = arma::ones<arma::Mat<T>>(blockSize, blockSize);
      arma::Col<T> zvec = arma::zeros<arma::Col<T>>(Nt);
      arma::Col<T> wvec = arma::zeros<arma::Col<T>>(Nt);

      for (size_t it = 0; it < newVecSize; it++)
      {
        Ublock = U[it];
        Sblock = S[it];
        Vblock = V[it];

        if (expWeighting) // Gaussian-weighted singular value thresholding
        {
          wvec = arma::abs(Sblock.max() * arma::exp(-0.5 * lambda * arma::square(Sblock)));
          pguresvt::SoftThreshold(Sthresh, Sblock, zvec, wvec);
        }
        else // Simple singular value thresholding
        {
          pguresvt::SoftThreshold(Sthresh, Sblock, zvec, lambda);
        }

        // Reconstruct from SVD
        block = Ublock * diagmat(Sthresh) * Vblock.t();

        for (size_t k = 0; k < Nt; k++)
        {
          int newY = patches(0, actualPatches(it), k);
          int newX = patches(1, actualPatches(it), k);

          v(arma::span(newY, newY + blockSize - 1),
            arma::span(newX, newX + blockSize - 1),
            arma::span(k, k)) += arma::reshape(block.col(k), blockSize, blockSize);

          weights(arma::span(newY, newY + blockSize - 1),
                  arma::span(newX, newX + blockSize - 1),
                  arma::span(k, k)) += weightsInc;
        }
      }

      v /= weights;                      // Apply weighting
      v.elem(find_nonfinite(v)).zeros(); // Handle divide-by-zero

      return v;
    };

  private:
    arma::icube patches;
    uint32_t Nx, Ny, Nt;
    uint32_t blockSize, blockOverlap;
    bool expWeighting;

    uint32_t vecSize, newVecSize, nxMbs, nyMbs, nxMbsDbo, nyMbsDbo;

    arma::uvec actualPatches;
    arma::Mat<T> block, Ublock, Vblock;
    arma::Col<T> Sblock, Sthresh;

    std::vector<arma::Mat<T>> U, V;
    std::vector<arma::Col<T>> S;
  };

} // namespace pguresvt

#endif
