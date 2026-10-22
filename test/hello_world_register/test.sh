#!/bin/bash

. ../common.sh

setup

./test  >test.log 2>&1
if [ $(< test.log ) == "Hello World!\n" ]; then
	output_passed
else
	output_failed
fi
