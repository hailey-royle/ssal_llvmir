#!/bin/bash

clean_dir () {
	rm test test.ll test.log test.txt >/dev/null 2>&1
}

output_passed () {
	printf "%-*s [\e[32m passed \e[0m]\n" 32 "${PWD##*/}" >>../test_results.log 2>&1
	clean_dir
	exit
}

output_failed () {
	printf "%-*s [\e[31m executable failed \e[0m]\n" 32 "${PWD##*/}" >>../test_results.log 2>&1
	exit
}

compile_sl () {
	../../ssal_llvmir test.ssal >test.log 2>&1
	if [ $? != 0 ]; then
		printf "%-*s [\e[31m ssal_llvir failed \e[0m]\n" 32 "${PWD##*/}" >>../test_results.log 2>&1
		exit
	fi
}

compile_ll () {
	clang test.ll ../../_start.s -o test -masm=intel -static -nostdlib >test.log 2>&1
	if [ $? != 0 ]; then
		printf "%-*s [\e[31m clang failed \e[0m]\n" 32 "${PWD##*/}" >>../test_results.log 2>&1
		exit
	fi
}

setup () {
	clean_dir
	compile_sl
	compile_ll
}

should_not_compile () {
	clean_dir
	../../ssal_llvmir test.ssal >test.log 2>&1
	if [ $? != 0 ]; then
		if [ ! -f test.ll ]; then
			output_passed
		fi
	fi
	printf "%-*s [\e[31m catch error failed \e[0m]\n" 32 "${PWD##*/}" >>../test_results.log 2>&1
}

