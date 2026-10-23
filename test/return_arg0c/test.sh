#!/bin/bash

. ../common.sh

setup

./test
if [ $? == 6 ]; then
	output_passed
else
	output_failed
fi
