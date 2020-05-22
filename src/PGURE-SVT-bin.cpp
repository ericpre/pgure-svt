/***************************************************************************

    PGURE-SVT Denoising

    Author: Tom Furnival
    Email:  tjof2@cam.ac.uk

    Copyright (C) 2015-2020 Tom Furnival

    This program uses Singular Value Thresholding (SVT) [1], combined
    with an unbiased risk estimator (PGURE) to denoise a video sequence
    of microscopy images [2]. Noise parameters for a mixed Poisson-Gaussian
    noise model are automatically estimated during the denoising.

    References:
    [1] "Unbiased Risk Estimates for Singular Value Thresholding and
        Spectral Estimators", (2013), Candes, EJ et al.
        http://dx.doi.org/10.1109/TSP.2013.2270464

    [2] "An Unbiased Risk Estimator for Image Denoising in the Presence
        of Mixed Poisson–Gaussian Noise", (2014), Le Montagner, Y et al.
        http://dx.doi.org/10.1109/TIP.2014.2300821

    This file is part of PGURE-SVT.

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

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <stdarg.h>
#include <stdexcept>
#include <stdlib.h>
#include <string>
#include <vector>
#include <armadillo>

namespace libtiff
{
#include "tiffio.h"
}

extern "C"
{
#include "medfilter.h"
}

#include "arps.hpp"
#include "hotpixel.hpp"
#include "params.hpp"
#include "noise.hpp"
#include "pgure.hpp"
#include "parallel.hpp"

// Little function to convert string "0"/"1" to boolean
bool strToBool(std::string const &s) { return s != "0"; };

// Main program
int main(int argc, char **argv)
{

  // Overall program timer
  auto overallstart = std::chrono::high_resolution_clock::now();

  // Print program header
  std::cout << std::endl
            << "PGURE-SVT Denoising" << std::endl
            << "Author: Tom Furnival" << std::endl
            << "Email:  tjof2@cam.ac.uk" << std::endl
            << std::endl;

  // Read in the parameter file name
  if (argc != 2)
  {
    std::cout << "  Usage: ./PGURE-SVT paramfile" << std::endl;
    return -1;
  }
  std::map<std::string, std::string> programOptions;
  std::ifstream paramFile(argv[1], std::ios::in);

  // Parse the parameter file
  ParseParameters(paramFile, programOptions);

  // Check all required parameters are specified
  if (programOptions.count("filename") == 0 ||
      programOptions.count("start_image") == 0 ||
      programOptions.count("end_image") == 0)
  {
    std::cout << "**WARNING** Required parameters not specified" << std::endl;
    std::cout << "            You must specify filename, start and end frame"
              << std::endl;
    return -1;
  }

  // Extract parameters
  // File path
  std::string filename = programOptions.at("filename");
  int lastindex = filename.find_last_of(".");
  std::string filestem = filename.substr(0, lastindex);

  // Frames to process
  int startimg = std::stoi(programOptions.at("start_image"));
  int endimg = std::stoi(programOptions.at("end_image"));
  int num_images = endimg - startimg + 1;

  // Move onto optional parameters
  // Patch size and trajectory length
  // Check sizes to ensure SVD is done right way round
  int Bs = (programOptions.count("patch_size") == 1)
               ? std::stoi(programOptions.at("patch_size"))
               : 4;
  // int Overlap = (programOptions.count("patch_overlap") == 1) ?
  // std::stoi(programOptions.at("patch_overlap")) : 1;
  int T = (programOptions.count("trajectory_length") == 1)
              ? std::stoi(programOptions.at("trajectory_length"))
              : 15;
  T = (Bs * Bs < T) ? (Bs * Bs) - 1 : T;
  std::string casoratisize = std::to_string(Bs * Bs) + "x" + std::to_string(T);

  // Noise parameters initialized at -1 unless user-defined
  double alpha = (programOptions.count("alpha") == 1)
                     ? std::stod(programOptions.at("alpha"))
                     : -1.;
  double mu = (programOptions.count("mu") == 1)
                  ? std::stod(programOptions.at("mu"))
                  : -1.;
  double sigma = (programOptions.count("sigma") == 1)
                     ? std::stod(programOptions.at("sigma"))
                     : -1.;

  // SVT thresholds and noise parameters initialized at -1 unless user-defined
  bool pgureOpt = (programOptions.count("pgure") == 1)
                      ? strToBool(programOptions.at("pgure"))
                      : true;
  double lambda;
  if (!pgureOpt)
  {
    if (programOptions.count("lambda") == 1)
    {
      lambda = std::stod(programOptions.at("lambda"));
    }
    else
    {
      std::cout << "**WARNING** PGURE optimization is turned OFF but no lambda "
                   "specified in parameter file"
                << std::endl;
      return -1;
    }
  }

  // Move onto advanced parameters
  // Motion neigbourhood size
  int MotionP = (programOptions.count("motion_neighbourhood") == 1)
                    ? std::stoi(programOptions.at("motion_neighbourhood"))
                    : 7;

  // Size of median filter
  int MedianSize = (programOptions.count("median_filter") == 1)
                       ? std::stoi(programOptions.at("median_filter"))
                       : 5;

  // PGURE tolerance
  double tol = 1E-7;
  if (programOptions.count("tolerance") == 1)
  {
    std::istringstream osTol(programOptions.at("tolerance"));
    double tol;
    osTol >> tol;
  }

  // Block overlap
  int Bo = (programOptions.count("patch_overlap") == 1)
               ? std::stoi(programOptions.at("patch_overlap"))
               : 1;

  // Noise method
  // TODO:tjof2 document this option
  int NoiseMethod = (programOptions.count("noise_method") == 1)
                        ? std::stoi(programOptions.at("noise_method"))
                        : 4;

  // Hot pixel threshold
  double hotpixelthreshold = (programOptions.count("hot_pixel") == 1)
                                 ? std::stoi(programOptions.at("hot_pixel"))
                                 : 10;

  // Check file exists
  std::string infilename = filestem + ".tif";
  if (!std::ifstream(infilename.c_str()))
  {
    std::cout << "**WARNING** File " << infilename << " not found" << std::endl;
    return -1;
  }

  // Load TIFF stack
  int tiffWidth, tiffHeight;
  unsigned short tiffDepth;
  libtiff::TIFF *MultiPageTiff = libtiff::TIFFOpen(infilename.c_str(), "r");
  libtiff::TIFFGetField(MultiPageTiff, TIFFTAG_IMAGEWIDTH, &tiffWidth);
  libtiff::TIFFGetField(MultiPageTiff, TIFFTAG_IMAGELENGTH, &tiffHeight);
  libtiff::TIFFGetField(MultiPageTiff, TIFFTAG_BITSPERSAMPLE, &tiffDepth);

  // Only work with square images
  if (tiffWidth != tiffHeight)
  {
    std::cout << "**WARNING** Frame dimensions are not square" << std::endl;
    return -1;
  }
  // Only work with 8-bit or 16-bit images
  if (tiffDepth != 8 && tiffDepth != 16)
  {
    std::cout << "**WARNING** Images must be 8-bit or 16-bit" << std::endl;
    return -1;
  }

  // Import the image sequence
  arma::cube inputsequence(tiffHeight, tiffWidth, 0);
  arma::cube filteredsequence(tiffHeight, tiffWidth, 0);
  if (MultiPageTiff)
  {
    int dircount = 0;
    int imgcount = 0;
    do
    {
      if (dircount >= (startimg - 1) && dircount <= (endimg - 1))
      {
        inputsequence.resize(tiffHeight, tiffWidth, imgcount + 1);
        filteredsequence.resize(tiffHeight, tiffWidth, imgcount + 1);

        unsigned short *Buffer = new unsigned short[tiffWidth * tiffHeight];
        unsigned short *FilteredBuffer =
            new unsigned short[tiffWidth * tiffHeight];

        for (int tiffRow = 0; tiffRow < tiffHeight; tiffRow++)
        {
          libtiff::TIFFReadScanline(MultiPageTiff, &Buffer[tiffRow * tiffWidth],
                                    tiffRow, 0);
        }

        arma::Mat<unsigned short> TiffSlice(Buffer, tiffHeight, tiffWidth);
        inplace_trans(TiffSlice);
        inputsequence.slice(imgcount) =
            arma::conv_to<arma::mat>::from(TiffSlice);

        // Apply median filter (constant-time) to the 8-bit image
        int memsize = 512 * 1024;  // L2 cache size
        int filtsize = MedianSize; // Median filter size in pixels
        ConstantTimeMedianFilter(Buffer, FilteredBuffer, tiffWidth, tiffHeight,
                                 tiffWidth, tiffWidth, filtsize, 1, memsize);
        arma::Mat<unsigned short> FilteredTiffSlice(FilteredBuffer, tiffHeight,
                                                    tiffWidth);
        inplace_trans(FilteredTiffSlice);
        filteredsequence.slice(imgcount) =
            arma::conv_to<arma::mat>::from(FilteredTiffSlice);
        imgcount++;
      }
      dircount++;
    } while (libtiff::TIFFReadDirectory(MultiPageTiff));
    libtiff::TIFFClose(MultiPageTiff);
  }
  // Is number of frames compatible?
  if (num_images > (int)inputsequence.n_slices)
  {
    std::cout << "**WARNING** Sequence only has " << inputsequence.n_slices
              << " frames" << std::endl;
    return -1;
  }

  // Copy image sequence and sizes
  arma::cube noisysequence = inputsequence;
  arma::cube cleansequence = inputsequence;
  cleansequence.zeros();

  // Get dimensions
  int Nx = tiffHeight;
  int Ny = tiffWidth;

  // Initial outlier detection (for hot pixels)
  // using median absolute deviation
  HotPixelFilter(noisysequence, hotpixelthreshold);

  // Print table headings
  int ww = 10;
  std::cout << std::endl;
  std::cout << std::right << std::setw(5 * ww + 5)
            << std::string(5 * ww + 5, '-') << std::endl;
  std::cout << std::setw(5) << "Frame" << std::setw(ww) << "Gain"
            << std::setw(ww) << "Offset" << std::setw(ww) << "Sigma"
            << std::setw(ww) << "Lambda" << std::setw(ww) << "Time (s)"
            << std::endl;
  std::cout << std::setw(5 * ww + 5) << std::string(5 * ww + 5, '-')
            << std::endl;

  // Loop over time windows
  int framewindow = std::floor(T / 2);

  auto &&func = [&, lambda_ = lambda](int timeiter) {
    auto lambda = lambda_;
    // Extract the subset of the image sequence
    arma::cube u(Nx, Ny, T), ufilter(Nx, Ny, T), v(Nx, Ny, T);
    if (timeiter < framewindow)
    {
      u = noisysequence.slices(0, 2 * framewindow);
      ufilter = filteredsequence.slices(0, 2 * framewindow);
    }
    else if (timeiter >= (num_images - framewindow))
    {
      u = noisysequence.slices(num_images - 2 * framewindow - 1,
                               num_images - 1);
      ufilter = filteredsequence.slices(num_images - 2 * framewindow - 1,
                                        num_images - 1);
    }
    else
    {
      u = noisysequence.slices(timeiter - framewindow, timeiter + framewindow);
      ufilter = filteredsequence.slices(timeiter - framewindow,
                                        timeiter + framewindow);
    }

    // Basic sequence normalization
    double inputmax = u.max();
    u /= inputmax;
    ufilter /= ufilter.max();

    // Perform noise estimation
    if (pgureOpt)
    {
      NoiseEstimator *noise = new NoiseEstimator;
      noise->Estimate(u, alpha, mu, sigma, 8, NoiseMethod);
      delete noise;
    }

    // Perform motion estimation
    MotionEstimator *motion = new MotionEstimator;
    motion->Estimate(ufilter, timeiter, framewindow, num_images, Bs, MotionP);
    arma::icube sequencePatches = motion->GetEstimate();
    delete motion;

    // Perform PGURE optimization
    PGURE *optimizer = new PGURE;
    optimizer->Initialize(u, sequencePatches, Bs, Bo, alpha, sigma, mu);
    // Determine optimum threshold value (max 1000 evaluations)
    if (pgureOpt)
    {
      lambda = (timeiter == 0) ? arma::accu(u) / (Nx * Ny * T) : lambda;
      lambda = optimizer->Optimize(tol, lambda, u.max(), 1E3);
      v = optimizer->Reconstruct(lambda);
    }
    else
    {
      v = optimizer->Reconstruct(lambda);
    }
    delete optimizer;

    // Rescale back to original range
    v *= inputmax;

    // Place frames back into sequence
    if (timeiter < framewindow)
    {
      cleansequence.slice(timeiter) = v.slice(timeiter);
    }
    else if (timeiter >= (num_images - framewindow))
    {
      int endseqFrame = timeiter - (num_images - T);
      cleansequence.slice(timeiter) = v.slice(endseqFrame);
    }
    else
    {
      cleansequence.slice(timeiter) = v.slice(framewindow);
    }
  };
  parallel(func, static_cast<unsigned long long>(num_images));

  // Finish the table off
  std::cout << std::setw(5 * ww + 5) << std::string(5 * ww + 5, '-')
            << std::endl
            << std::endl;

  // Normalize to [0,65535] range
  cleansequence = (cleansequence - cleansequence.min()) /
                  (cleansequence.max() - cleansequence.min());
  arma::Cube<unsigned short> outTiff(tiffWidth, tiffHeight, num_images);
  outTiff =
      arma::conv_to<arma::Cube<unsigned short>>::from(65535 * cleansequence);

  // Get the filename
  std::string outfilename = filestem + "-CLEANED.tif";

  // Set the output file headers
  libtiff::TIFF *MultiPageTiffOut = libtiff::TIFFOpen(outfilename.c_str(), "w");
  libtiff::TIFFSetField(MultiPageTiffOut, TIFFTAG_IMAGEWIDTH, tiffWidth);
  libtiff::TIFFSetField(MultiPageTiffOut, TIFFTAG_IMAGELENGTH, tiffHeight);
  libtiff::TIFFSetField(MultiPageTiffOut, TIFFTAG_BITSPERSAMPLE, 16);

  // Write the file
  if (!MultiPageTiffOut)
  {
    std::cout << "**WARNING** File " << outfilename << " could not be written"
              << std::endl;
    return -1;
  }

  for (int tOut = 0; tOut < num_images; tOut++)
  {
    libtiff::TIFFSetField(MultiPageTiffOut, TIFFTAG_IMAGEWIDTH, tiffWidth);
    libtiff::TIFFSetField(MultiPageTiffOut, TIFFTAG_IMAGELENGTH, tiffHeight);
    libtiff::TIFFSetField(MultiPageTiffOut, TIFFTAG_BITSPERSAMPLE, 16);
    libtiff::TIFFSetField(MultiPageTiffOut, TIFFTAG_SAMPLESPERPIXEL, 1);
    libtiff::TIFFSetField(MultiPageTiffOut, TIFFTAG_PLANARCONFIG,
                          PLANARCONFIG_CONTIG);
    libtiff::TIFFSetField(MultiPageTiffOut, TIFFTAG_PHOTOMETRIC,
                          PHOTOMETRIC_MINISBLACK);
    libtiff::TIFFSetField(MultiPageTiffOut, TIFFTAG_ORIENTATION,
                          ORIENTATION_TOPLEFT);

    libtiff::TIFFSetField(MultiPageTiffOut, TIFFTAG_SUBFILETYPE, FILETYPE_PAGE);
    libtiff::TIFFSetField(MultiPageTiffOut, TIFFTAG_PAGENUMBER, tOut,
                          num_images);
    for (int tiffRow = 0; tiffRow < tiffHeight; tiffRow++)
    {
      arma::Mat<unsigned short> outSlice = outTiff.slice(tOut);
      inplace_trans(outSlice);
      unsigned short *OutBuffer = outSlice.memptr();
      libtiff::TIFFWriteScanline(MultiPageTiffOut,
                                 &OutBuffer[tiffRow * tiffWidth], tiffRow, 0);
    }
    libtiff::TIFFWriteDirectory(MultiPageTiffOut);
  }
  libtiff::TIFFClose(MultiPageTiffOut);

  // Overall program timer
  auto overallend = std::chrono::high_resolution_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      overallend - overallstart);
  std::cout << "Total time: " << std::setprecision(5) << (elapsed.count() / 1E6)
            << " seconds" << std::endl
            << std::endl;

  return 0;
}
