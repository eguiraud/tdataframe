#!/bin/bash

echo "building test executables..."
pushd ../tests > /dev/null
make
RES=$?
if (( $RES > 0 )); then
   echo "error building tests"
   exit 1
fi
popd > /dev/null

echo "checking executables..."
FILES=(test_misc testIMT tdf001_introduction tdf002_dataModel regression_multipletriggerrun \
       test_functiontraits regression_zeroentries test_branchoverwrite)
RETCODE=0
for F in ${FILES[@]}; do
   ../tests/$F | diff $F.out -
   RES=$?
   if (( $RES == 1)); then
      echo "*********** output for test $F changed!"
      RETCODE=1
   elif (( $RES == 2)); then
      echo "*********** something went wrong diffing output of test $F!"
      RETCODE=2
   fi
done

echo "checking macros..."
MACROS=(test_ctors)
for M in ${MACROS[@]}; do
   root -b -l -n -q -x ../tests/${M}.cxx | diff ${M}.out -
   RES=$?
   if (( $RES == 1)); then
      echo "*********** output for test $M changed!"
      RETCODE=1
   elif (( $RES == 2)); then
      echo "*********** something went wrong diffing output of test $M!"
      RETCODE=2
   fi
done

if (( $RETCODE == 0 )); then
   echo "everything fine!"
fi

#rm -f *.root
exit $RETCODE

