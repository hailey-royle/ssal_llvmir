#!/bin/bash

. ../common.sh

setup

./test
if [ $? == 4 ]; then
	output_passed
else
	output_failed
fi
