#!/bin/bash

. ../common.sh

setup

./test
if [ $? == 16 ]; then
	output_passed
else
	output_failed
fi
