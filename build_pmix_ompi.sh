#!/bin/sh

TOP_DIR=`pwd`

# Hack alert.  Use the Intel proxy if the host exists, else connect direct.
host proxy-chain.intel.com > /dev/null 2>&1
[ $? -eq 0 ] && export https_proxy=http://proxy-chain.intel.com:911

set -e

cd ..

HWLOC_VERSION=1.11.2
[ -d hwloc-$HWLOC_VERSION ] && /bin/rm -rf hwloc-$HWLOC_VERSION
[ -f hwloc-${HWLOC_VERSION}.tar.gz ] || wget https://www.open-mpi.org/software/hwloc/v1.11/downloads/hwloc-${HWLOC_VERSION}.tar.gz
tar -xzf hwloc-${HWLOC_VERSION}.tar.gz
cd hwloc-$HWLOC_VERSION
./configure --prefix=$TOP_DIR/install
make
make install
cd ..

cd pmix
./autogen.sh
./configure --with-platform=optimized --prefix=$TOP_DIR/install --with-libevent=/usr --with-hwloc=$TOP_DIR/install
make
make install
cd ..

cd ompi
./autogen.pl
./configure --prefix=$TOP_DIR/install --with-pmix=$TOP_DIR/install --with-libevent=/usr --disable-mpi-fortran --with-hwloc=$TOP_DIR/install
make
make install
