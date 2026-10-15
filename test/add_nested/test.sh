#!/bin/bash

. ../common.sh

setup

./test
if [ $? == 23 ]; then
	output_passed
else
	output_failed
fi
