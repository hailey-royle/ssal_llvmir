#!/bin/bash

. ../common.sh

setup

./test
if [ $? == 30 ]; then
	output_passed
else
	output_failed
fi
