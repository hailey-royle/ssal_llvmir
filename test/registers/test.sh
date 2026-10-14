#!/bin/bash

. ../common.sh

setup

./test
if [ $? == 2 ]; then
	output_passed
else
	output_failed
fi
