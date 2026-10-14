#!/bin/bash

. ../common.sh

setup

./test
if [ $? == 0 ]; then
	output_passed
else
	output_failed
fi
