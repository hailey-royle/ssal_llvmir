#!/bin/bash

. ../common.sh

setup

./test two asldjflasdfj four 5p p
if [ $? == 6 ]; then
	output_passed
else
	output_failed
fi
