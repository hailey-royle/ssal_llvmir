#!/bin/bash

. ../common.sh

setup

./test 1 2 3 four >test.log 2>&1

if cmp --silent -- ./test.log <(echo -ne './test123four'); then
	output_passed
else
	output_failed
fi

