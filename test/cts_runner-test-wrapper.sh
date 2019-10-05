#!/bin/sh

set -e

mode=$1
shift
runner=$1
shift
expected=$1
shift

output=`mktemp`

$runner "$@" --output $output -- --deqp-mock-mode=$mode

diff -u $expected $output

rm $output
