#!/bin/sh


FLIST="-s SConstruct -s src/SConscript -s src/utest/SConscript"

FILE=`ls -1 proto/*/SConscript`
for FNAME in $FILE
do
  FLIST+=" -s $FNAME"
done

FILE=`ls -1 test/*.py`
for FNAME in $FILE
do
  FLIST+=" -P3 $FNAME"
done

./scons_local/check_python.sh $FLIST
if [ $? -ne 0 ]; then
  exit 1
fi
exit 0
