#!/bin/sh
#   This script builds from source:
#       - Armadillo 9.800.2
#       - NLopt 2.6.1
#
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

set -ex
mkdir build_deps
cd build_deps

# Armadillo
wget http://sourceforge.net/projects/arma/files/armadillo-9.800.2.tar.xz
tar -xvf armadillo-9.800.2.tar.xz > log-file 2>&1
cd armadillo-9.800.2
cmake .
make
sudo make install
cd ../

# NLopt
wget https://github.com/stevengj/nlopt/archive/v2.6.2.tar.gz
tar -xzvf v2.6.2.tar.gz > log-file 2>&1
cd nlopt-2.6.2
cmake .
make
sudo make install
cd ../

# Tidy-up
cd ../
sudo rm -rf build_deps/
sudo ldconfig
