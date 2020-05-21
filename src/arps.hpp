/***************************************************************************

    Copyright (C) 2015-2020 Tom Furnival

    Perform Adaptive Rood Pattern Search (ARPS) for motion estimation [1].
    Based on MATLAB code by Aroh Barjatya [2].

    References:
    [1]     "Adaptive rood pattern search for fast block-matching motion
            estimation", (2002), Nie, Y and Kai-Kuang, M
            http://dx.doi.org/10.1109/TIP.2002.806251

    [2]     http://uk.mathworks.com/matlabcentral/fileexchange/8761-block-matching-algorithms-for-motion-estimation

    This file is part of  PGURE-SVT.

    PGURE-SVT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    PGURE-SVT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PGURE-SVT. If not, see <http://www.gnu.org/licenses/>.

***************************************************************************/

#ifndef ARPS_HPP
#define ARPS_HPP

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <armadillo>

class MotionEstimator
{
public:
  MotionEstimator() {}
  ~MotionEstimator() {}

  void Estimate(const arma::cube &A, const int iter,
                const int timeWindow, const int nImages,
                const int blockSize, const int motionWindow)
  {
    Nx = A.n_rows;
    Ny = A.n_cols;
    T = A.n_slices;
    wind = motionWindow;
    Bs = blockSize;
    OoBlockSizeSq = 1.0 / (Bs * Bs);
    vecSize = (1 + (Nx - Bs)) * (1 + (Ny - Bs));

    patches = arma::zeros<arma::icube>(2, vecSize, 2 * timeWindow + 1);
    motions = arma::zeros<arma::icube>(2, vecSize, 2 * timeWindow);

    // Perform motion estimation
    // Complicated for cases near beginning and end of sequence
    if (iter < timeWindow)
    {
      // Populate reference frame coordinates
      for (int i = 0; i < vecSize; i++)
      {
        patches(0, i, iter) = i % (1 + (Ny - Bs));
        patches(1, i, iter) = i / (1 + (Nx - Bs));
      }
      // Perform motion estimation
      // Go forwards
      for (int i = 0; i < T - iter - 1; i++)
      {
        ARPSMotionEstimation(A, i, iter + i, iter + i + 1, iter + i);
      }
      // Go backwards
      for (int i = -1; i >= -iter; i--)
      {
        ARPSMotionEstimation(A, i, iter + i + 1, iter + i, iter + i + 1);
      }
    }
    else if (iter >= (nImages - timeWindow))
    {
      int endseqFrame = iter - (nImages - T);
      // Populate reference frame coordinates
      for (int i = 0; i < vecSize; i++)
      {
        patches(0, i, endseqFrame) = i % (1 + (Ny - Bs));
        patches(1, i, endseqFrame) = i / (1 + (Nx - Bs));
      }
      // Perform motion estimation
      // Go forwards
      for (int i = 0; i < 2 * timeWindow - endseqFrame; i++)
      {
        ARPSMotionEstimation(A, i, endseqFrame + i, endseqFrame + i + 1,
                             endseqFrame + i);
      }
      // Go backwards
      for (int i = -1; i >= -endseqFrame; i--)
      {
        if (2 * (int)timeWindow == endseqFrame)
        {
          ARPSMotionEstimation(A, i, endseqFrame + i + 1, endseqFrame + i,
                               endseqFrame + i);
        }
        else
        {
          ARPSMotionEstimation(A, i, endseqFrame + i + 1, endseqFrame + i,
                               endseqFrame + i + 1);
        }
      }
    }
    else
    {
      // Populate reference frame coordinates
      for (int i = 0; i < vecSize; i++)
      {
        patches(0, i, timeWindow) = i % (1 + (Ny - Bs));
        patches(1, i, timeWindow) = i / (1 + (Nx - Bs));
      }
      // Perform motion estimation
      // Go forwards
      for (int i = 0; i < timeWindow; i++)
      {
        ARPSMotionEstimation(A, i, timeWindow + i, timeWindow + i + 1,
                             timeWindow + i);
      }
      // Go backwards
      for (int i = -1; i >= -timeWindow; i--)
      {
        ARPSMotionEstimation(A, i, timeWindow + i + 1, timeWindow + i,
                             timeWindow + i + 1);
      }
    }
    return;
  }

  arma::icube GetEstimate() { return patches; }

private:
  arma::icube patches, motions;
  int Nx, Ny, T, Bs, vecSize, wind;
  double OoBlockSizeSq;

  // Adaptive Rood Pattern Search (ARPS) method
  void ARPSMotionEstimation(const arma::cube &A, const int curFrame,
                            const int iARPS1, const int iARPS2, const int iARPS3)
  {
    double norm = 0;
    arma::vec costs = arma::ones<arma::vec>(6) * 1E8;
    arma::umat checkMat = arma::zeros<arma::umat>(2 * wind + 1, 2 * wind + 1);
    arma::imat LDSP = arma::zeros<arma::imat>(6, 2);
    arma::imat SDSP = arma::zeros<arma::imat>(5, 2);

    for (int it = 0; it < vecSize; it++)
    {
      costs.ones();
      costs *= 1E8;
      checkMat.zeros();
      LDSP.zeros();
      SDSP.zeros();

      SDSP(0, 0) = 0;
      SDSP(0, 1) = -1;
      SDSP(1, 0) = -1;
      SDSP(1, 1) = 0;
      SDSP(2, 0) = 0;
      SDSP(2, 1) = 0;
      SDSP(3, 0) = 1;
      SDSP(3, 1) = 0;
      SDSP(4, 0) = 0;
      SDSP(4, 1) = 1;
      LDSP.rows(arma::span(0, 4)) = SDSP;

      int i = it % (1 + (Nx - Bs));
      int j = it / (1 + (Ny - Bs));

      int x = (int)j;
      int y = (int)i;

      arma::cube refBlock = A(arma::span(i, i + Bs - 1),
                              arma::span(j, j + Bs - 1),
                              arma::span(iARPS1));

      arma::cube newBlock = A(arma::span(i, i + Bs - 1),
                              arma::span(j, j + Bs - 1),
                              arma::span(iARPS2));

      norm = arma::norm(refBlock.slice(0) - newBlock.slice(0), "fro");
      costs(2) = norm * norm * OoBlockSizeSq;

      checkMat(wind, wind) = 1;

      int stepSize;
      int maxIdx;

      if (j == 0)
      {
        stepSize = 2;
        maxIdx = 5;
      }
      else
      {
        int yTmp = std::abs(motions(0, it, iARPS3));
        int xTmp = std::abs(motions(1, it, iARPS3));
        stepSize = (xTmp <= yTmp) ? yTmp : xTmp;
        if ((xTmp == stepSize && yTmp == 0) || (xTmp == 0 && yTmp == stepSize))
        {
          maxIdx = 5;
        }
        else
        {
          maxIdx = 6;
          LDSP(5, 0) = motions(1, it, iARPS3);
          LDSP(5, 1) = motions(0, it, iARPS3);
        }
      }
      LDSP(0, 0) = 0;
      LDSP(0, 1) = -stepSize;
      LDSP(1, 0) = -stepSize;
      LDSP(1, 1) = 0;
      LDSP(2, 0) = 0;
      LDSP(2, 1) = 0;
      LDSP(3, 0) = stepSize;
      LDSP(3, 1) = 0;
      LDSP(4, 0) = 0;
      LDSP(4, 1) = stepSize;

      // Currently not used, but motion estimation can be predictive
      // if this value is larger than 0!
      double pMotion = 0.0;

      // Do the LDSP
      for (int k = 0; k < maxIdx; k++)
      {
        int refBlkVer = y + LDSP(k, 1);
        int refBlkHor = x + LDSP(k, 0);
        if (refBlkHor < 0 || refBlkHor + Bs - 1 >= Ny || refBlkVer < 0 || refBlkVer + Bs - 1 >= Nx)
        {
          continue;
        }
        else if (k == 2 || stepSize == 0)
        {
          continue;
        }
        else
        {
          arma::cube powBlock = A(arma::span(refBlkVer, refBlkVer + Bs - 1),
                                  arma::span(refBlkHor, refBlkHor + Bs - 1),
                                  arma::span(iARPS2));
          if (curFrame == 0)
          {
            norm = arma::norm(refBlock.slice(0) - powBlock.slice(0), "fro");
            costs(k) = norm * norm * OoBlockSizeSq;
          }
          else if (curFrame < 0)
          {
            norm = arma::norm(refBlock.slice(0) - powBlock.slice(0), "fro");
            costs(k) = norm * norm * OoBlockSizeSq;

            if (pMotion > 0.0)
            {
              arma::ivec predPos = arma::vectorise(
                  patches(arma::span(), arma::span(it), arma::span(iARPS1)) -
                  motions(arma::span(), arma::span(it), arma::span(iARPS3)));
              costs(k) += pMotion * std::sqrt(std::pow(predPos(0) - refBlkVer, 2) +
                                              std::pow(predPos(1) - refBlkHor, 2));
            }
          }
          else if (curFrame > 0)
          {
            norm = arma::norm(refBlock.slice(0) - powBlock.slice(0), "fro");
            costs(k) = norm * norm * OoBlockSizeSq;

            if (pMotion > 0.0)
            {
              arma::ivec predPos = arma::vectorise(
                  patches(arma::span(), arma::span(it), arma::span(iARPS1)) +
                  motions(arma::span(), arma::span(it), arma::span(iARPS3)));

              norm = arma::norm(refBlock.slice(0) - powBlock.slice(0), "fro");
              costs(k) += pMotion * std::sqrt(std::pow(predPos(0) - refBlkVer, 2) +
                                              std::pow(predPos(1) - refBlkHor, 2));
            }
          }

          checkMat(LDSP(k, 1) + wind, LDSP(k, 0) + wind) = 1;
        }
      }

      arma::uvec point = arma::find(costs == costs.min());
      x += LDSP(point(0), 0);
      y += LDSP(point(0), 1);
      double cost = costs.min();
      costs.ones();
      costs *= 1E8;
      costs(2) = cost;

      // Do the SDSP
      bool doneFlag = false;

      do
      {
        for (int k = 0; k < 5; k++)
        {
          int refBlkVer = y + SDSP(k, 1);
          int refBlkHor = x + SDSP(k, 0);

          if (refBlkHor < 0 || refBlkHor + Bs - 1 >= Ny || refBlkVer < 0 || refBlkVer + Bs - 1 >= Nx)
          {
            continue;
          }
          else if (k == 2)
          {
            continue;
          }
          else if (refBlkHor < (int)(j - wind) || refBlkHor > (int)(j + wind) ||
                   refBlkVer < (int)(i - wind) || refBlkVer > (int)(i + wind))
          {
            continue;
          }
          else if (checkMat(y - i + SDSP(k, 1) + wind, x - j + SDSP(k, 0) + wind) == 1)
          {
            continue;
          }
          else
          {
            arma::cube powBlock = A(arma::span(refBlkVer, refBlkVer + Bs - 1),
                                    arma::span(refBlkHor, refBlkHor + Bs - 1),
                                    arma::span(iARPS2));
            if (curFrame == 0)
            {
              norm = arma::norm(refBlock.slice(0) - powBlock.slice(0), "fro");
              costs(k) = norm * norm * OoBlockSizeSq;
            }
            else if (curFrame < 0)
            {
              norm = arma::norm(refBlock.slice(0) - powBlock.slice(0), "fro");
              costs(k) = norm * norm * OoBlockSizeSq;

              if (pMotion > 0.0)
              {
                arma::ivec predPos = arma::vectorise(
                    patches(arma::span(), arma::span(it), arma::span(iARPS1)) -
                    motions(arma::span(), arma::span(it), arma::span(iARPS3)));

                costs(k) += pMotion * std::sqrt(std::pow(predPos(0) - refBlkVer, 2) +
                                                std::pow(predPos(1) - refBlkHor, 2));
              }
            }
            else if (curFrame > 0)
            {
              norm = arma::norm(refBlock.slice(0) - powBlock.slice(0), "fro");
              costs(k) = norm * norm * OoBlockSizeSq;

              if (pMotion > 0.0)
              {
                arma::ivec predPos = arma::vectorise(
                    patches(arma::span(), arma::span(it), arma::span(iARPS1)) +
                    motions(arma::span(), arma::span(it), arma::span(iARPS3)));

                costs(k) += pMotion * std::sqrt(std::pow(predPos(0) - refBlkVer, 2) +
                                                std::pow(predPos(1) - refBlkHor, 2));
              }
            }

            checkMat(y - i + SDSP(k, 1) + wind, x - j + SDSP(k, 0) + wind) = 1;
          }
        }
        point = arma::find(costs == costs.min());
        cost = costs.min();

        if (point(0) == 2)
        {
          doneFlag = true;
        }
        else
        {
          x += SDSP(point(0), 0);
          y += SDSP(point(0), 1);
          costs.ones();
          costs *= 1E8;
          costs(2) = cost;
        }
      } while (!doneFlag);

      motions(0, it, iARPS3) = y - i;
      motions(1, it, iARPS3) = x - j;
      patches(0, it, iARPS2) = y;
      patches(1, it, iARPS2) = x;
    }
    return;
  }
};

#endif
