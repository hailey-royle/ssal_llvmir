#!/bin/bash

. ../common.sh

setup

./test >test.log 2>&1

if cmp --silent -- ./test.log <(echo -ne 'Hello World!\n'); then
	output_passed
else
	output_failed
fi

