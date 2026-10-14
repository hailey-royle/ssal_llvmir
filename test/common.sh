#!/bin/bash

clean_dir () {
	rm test test.ssal.ll test.log test.txt >/dev/null 2>&1
}

output_passed () {
	printf "%-*s [\e[32m passed \e[0m]\n" 24 "${PWD##*/}" >>../test_results.log 2>&1
	clean_dir
	exit
}

output_failed () {
	printf "%-*s [\e[31m executable failed \e[0m]\n" 24 "${PWD##*/}"
	exit
}

compile_sl () {
	../../ssal_llvmir test.ssal >test.log 2>&1
	if [ $? != 0 ]; then
		printf "%-*s [\e[31m ssal_llvir failed \e[0m]\n" 24 "${PWD##*/}"
		exit
	fi
}

compile_ll () {
	clang test.ssal.ll -o test >test.log 2>&1
	if [ $? != 0 ]; then
		printf "%-*s [\e[31m clang failed \e[0m]\n" 24 "${PWD##*/}"
		exit
	fi
}

setup () {
	clean_dir
	compile_sl
	compile_ll
}


