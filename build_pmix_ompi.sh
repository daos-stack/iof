#!/bin/sh

TOP_DIR=`pwd`

# Hack alert.  Use the Intel proxy if the host exists, else connect direct.
host proxy-chain.intel.com > /dev/null 2>&1
[ $? -eq 0 ] && export https_proxy=http://proxy-chain.intel.com:911

set -e

cd ..

LIBEVENT=libevent-2.0.22-stable
rm -rf libevent*
wget https://github.com/libevent/libevent/releases/download/release-2.0.22-stable/libevent-2.0.22-stable.tar.gz
tar -xvzf libevent-2.0.22-stable.tar.gz
cd ${LIBEVENT}
./configure --prefix=$TOP_DIR/install
make
make install
cd ..

rm -rf hwloc*
wget https://www.open-mpi.org/software/hwloc/v1.11/downloads/hwloc-1.11.2.tar.gz
tar -xvzf hwloc-1.11.2.tar.gz
HWLOC=hwloc-1.11.2
cd ${HWLOC}
./configure --prefix=$TOP_DIR/install
make
make install
cd ..

cd pmix
./autogen.sh
./configure --with-platform=optimized --prefix=$TOP_DIR/install --with-libevent=$TOP_DIR/install --with-hwloc=$TOP_DIR/install
make
make install
cd ..

cd ompi
./autogen.pl
./configure --prefix=$TOP_DIR/install --with-pmix=$TOP_DIR/install --with-libevent=$TOP_DIR/install --disable-mpi-fortran --with-hwloc=$TOP_DIR/install
make
make install
