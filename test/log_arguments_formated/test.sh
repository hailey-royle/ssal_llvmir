#!/bin/bash

. ../common.sh

setup

./test 1 2 3 four >test.log 2>&1

if cmp --silent -- ./test.log <(echo -ne 'Arguments:\n  0:  ./test\n  1:  1\n  2:  2\n  3:  3\n  4:  four\n'); then
	output_passed
else
	output_failed
fi

