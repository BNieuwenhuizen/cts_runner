#!/bin/sh

mode=$1
shift
runner=$1
shift
expected=$1
shift
expected_exitcode=$1
shift

output=`mktemp`

$runner "$@" --output $output -- --deqp-mock-mode=$mode
exitcode=$?

if ! diff -u $expected $output; then
  exit 1
fi

if [ $exitcode != $expected_exitcode ]; then
   echo "Runner returned $exitcode instead of $expected_exitcode"
   exit 1
fi

rm $output
