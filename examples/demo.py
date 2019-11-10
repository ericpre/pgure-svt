# -*- coding: utf-8 -*-
# Copyright 2015-2019 Tom Furnival
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

import numpy as np
from pguresvt import pguresvt

import hyperspy.api as hs
from hyperspy.hspy import *
import matplotlib.pyplot as plt

# Load example dataset
movie = hs.load("../test/examplesequence.tif")
X = np.transpose(movie.data)
# X = X[:,:,0:15]

# Initialize with default parameters
svt = pguresvt.SVT(patchsize=4, patchoverlap=2, length=15, threshold=0.5, tol=1e-6)

# Run the denoising
svt.denoise(X)

im = hs.signals.Image(np.transpose(svt.Y))
im.plot(navigator="slider")
