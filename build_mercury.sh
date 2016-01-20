#!/bin/sh

set -e
set -x

TOP_DIR=`pwd`

cd build
ln -s `pwd`/../../mercury mercury
ln -s `pwd`/../../bmi bmi
ln -s `pwd`/../../openpa openpa

cd bmi
./prepare
./configure --enable-shared --prefix=$TOP_DIR/install/
make
make install
cd ..

cd openpa/
libtoolize
./autogen.sh
./configure --prefix $TOP_DIR/install
make
make install
cd ..

mkdir -p mercury-build
cd mercury-build
cmake -DOPA_LIBRARY=$TOP_DIR/install/lib/libopa.a \
    -DOPA_INCLUDE_DIR=$TOP_DIR/install/include/ \
    -DCMAKE_INSTALL_PREFIX=$TOP_DIR/install \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_SHARED_LIBS=ON \
    -DMERCURY_BUILD_HL_LIB=ON \
    -DMERCURY_USE_BOOST_PP=ON \
    -DNA_USE_BMI=ON \
    -DBUILD_TESTING=ON \
    -DBUILD_DOCUMENTATION=OFF ../../../mercury
make
make install
make test
