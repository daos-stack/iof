#!/bin/sh

set -e

./scons_local/check_python.sh -s SConstruct
./scons_local/check_python.sh -s src/SConscript

FILE=`ls -1 proto/*/SConscript`
for FNAME in $FILE
do
    ./scons_local/check_python.sh -s $FNAME
done
