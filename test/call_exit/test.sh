#!/bin/bash

. ../common.sh

setup

./test
if [ $? == 42 ]; then
	output_passed
else
	output_failed
fi
