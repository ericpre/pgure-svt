# Copyright 2015-2020 Tom Furnival
#
# This file is part of PGURE-SVT.
#
# PGURE-SVT is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# PGURE-SVT is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with PGURE-SVT.  If not, see <http://www.gnu.org/licenses/>.

import ctypes
import multiprocessing

import numpy as np
from numpy.ctypeslib import ndpointer


def _addnoise(x, alpha, mu, sigma):
    """Add Poisson-Gaussian noise to the data x

    Parameters
    ----------
    x : float
        The original data

    alpha : float
        Level of noise gain

    mu : float
        Level of noise offset

    sigma : float
        Level of Gaussian noise

    Returns
    -------
    y : float
        The corrupted data

    """
    y = alpha * np.random.poisson(x / alpha) + mu + sigma * np.random.randn()
    return y


def PoissonGaussianNoiseGenerator(X, alpha=0.1, mu=0.1, sigma=0.1):
    """Add Poisson-Gaussian noise to the data X

    Parameters
    ----------
    X : array
        The data to be corrupted

    alpha : float
        Level of noise gain

    mu : float
        Level of noise offset

    sigma : float
        Level of Gaussian noise

    Returns
    -------
    Y : array
        The corrupted data

    """
    # Do some error checking
    if alpha < 0.0 or alpha > 1.0:
        raise ValueError("alpha should be in range [0,1]")
    # Vectorize noise function
    addnoise = np.vectorize(_addnoise, otypes=[np.float])

    # Rescale to [0,1] range
    Xmax = np.amax(X)
    X = X / Xmax

    # Add noise
    Y = addnoise(X, alpha, mu, sigma)

    # Rescale to [0,1] range
    Y = Y + np.abs(np.amin(Y))

    # Rescale to X range
    Y = Xmax * Y / np.amax(Y)
    return Y


class SVT(object):
    """
    Parameters
    ----------
    patchsize : integer
        The dimensions of the patch in pixels
        to form a Casorati matrix (default = 4)

    length : integer
        Length in frames of the block to form
        a Casorati matrix. Must be odd (default = 15)

    optimize : bool
        Whether to optimize PGURE or just denoise
        according to given threshold (default = True)

    threshold : float
        Threshold to use if not optimizing PGURE
        (default = 0.5)

    alpha : float
        Level of noise gain, if negative then
        estimated online (default = -1)

    mu : float
        Level of noise offset, if negative then
        estimated online (default = -1)

    sigma : float
        Level of Gaussian noise, if negative then
        estimated online (default = -1)

    arpssize : integer
        Size of neighbourhood for ARPS search
        Must be odd
        (default = 7 pixels)

    tol : float
        Tolerance of PGURE optimizers
        (default = 1E-7)

    median : integer
        Size of initial median filter
        Must be odd
        (default = 5 pixels)

    """

    def __init__(
        self,
        patchsize=4,
        patchoverlap=1,
        length=15,
        optimize=True,
        threshold=0.5,
        estimatenoise=True,
        alpha=-1.0,
        mu=-1.0,
        sigma=-1.0,
        arpssize=7,
        tol=1e-7,
        median=5,
        hotpixelthreshold=10,
        numthreads=1,
    ):

        # Load up parameters
        self.patchsize = patchsize
        self.overlap = patchoverlap
        self.length = length
        self.optimize = optimize
        self.threshold = threshold
        self.estimation = estimatenoise
        self.alpha = alpha
        self.mu = mu
        self.sigma = sigma
        self.arpssize = arpssize
        self.tol = tol
        self.median = median
        self.hotpixelthreshold = hotpixelthreshold
        self.numthreads = numthreads

        # Do some error checking
        if self.overlap > self.patchsize:
            raise ValueError("Patch overlap should not be greater than patch size")
        if self.arpssize % 2 == 0:
            raise ValueError("ARPS motion estimation window size should be odd")
        if self.threshold < 0.0 or self.threshold > 1.0:
            raise ValueError("Threshold should be in range [0,1]")
        if self.median % 2 == 0:
            raise ValueError("Median filter size should be odd")

        # Check number of available CPU cores
        num_cpu_cores = multiprocessing.cpu_count()
        if self.numthreads > num_cpu_cores:
            raise ValueError(
                "Number of threads should be less than or equal to %d" % num_cpu_cores
            )

        # Setup ctypes function
        self._PGURESVT = ctypes.cdll.LoadLibrary(
            "${PYTHONLIBRARYPATH}/libpguresvt.so"
        ).PGURESVT
        self._PGURESVT.restype = ctypes.c_int
        self._PGURESVT.argtypes = [
            ndpointer(ctypes.c_double, flags="F"),
            ndpointer(ctypes.c_double, flags="F"),
            ndpointer(ctypes.c_int),
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_bool,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_int,
            ctypes.c_double,
            ctypes.c_int,
            ctypes.c_double,
            ctypes.c_int,
        ]

        self.Y = None

    def denoise(self, X):
        """Denoise the data X

        Parameters
        ----------
        X : array [nx, ny, time]
            The image sequence to be denoised

        Returns
        -------
        self : object
            Returns the instance itself

        """

        self._denoise(X)
        return self

    def _denoise(self, X):
        """Denoise the data X

        Parameters
        ----------
        X : array [nx, ny, time]
            The image sequence to be denoised

        Returns
        -------
        Y : array [nx, ny, time]
            Returns the denoised sequence

        """
        # Check X is Fortran-order
        X = self._check_array(X)
        # Check sequence dimensions
        dims = np.asarray(X.shape).astype(np.int32)
        if dims[0] != dims[1] and self.estimation:
            raise ValueError("Quadtree noise estimation requires square images")
        if self._is_power_of_two(dims[0]) is False and self.estimation:
            raise ValueError("Quadtree noise estimation requires image dimensions 2^N")
        # Create Y in memory
        Y = np.zeros(X.shape, dtype=np.double, order="F")
        _ = self._PGURESVT(
            X,
            Y,
            dims,
            self.patchsize,
            self.overlap,
            self.length,
            self.optimize,
            self.threshold,
            self.alpha,
            self.mu,
            self.sigma,
            self.arpssize,
            self.tol,
            self.median,
            self.hotpixelthreshold,
            self.numthreads,
        )
        self.Y = Y
        return Y

    def _check_array(self, X):
        """Sanity-checks the data and parameters.

        Parameters
        ----------
        X : array [nx, ny, time]
            The data as an array

        Returns
        -------
        x : array [nx, ny, time]
            Returns the array in Fortran-order (column-major)

        """
        x = np.copy(X.astype(np.double), order="F")
        return x

    def _is_power_of_two(self, n):
        n = n / 2
        if n == 2:
            return True
        elif n > 2:
            return self._is_power_of_two(n)
        else:
            return False
