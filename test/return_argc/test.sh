#!/bin/bash

. ../common.sh

setup

./test two 3
if [ $? == 3 ]; then
	output_passed
else
	output_failed
fi
