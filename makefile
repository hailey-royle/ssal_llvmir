SHELL := /bin/bash
# t := test_name

build:
	time clang ssal_llvmir.c -o ssal_llvmir -Wall -Wextra -std=c89 -O3

debug:
	time clang ssal_llvmir.c -o ssal_llvmir -Wall -Wextra -std=c89 -Og -ggdb

_start:
	time clang -c _start.s -o _start.o -masm=intel

test: debug _start
ifdef t
	./ssal_llvmir test/$(t)/test.ssal
	clang test/$(t)/test.ssal.ll _start.o -o $(t) -static -nostdlib
else
	time $(SHELL) -c "cd test && ./test.sh"
endif

.PHONY: build debug test clean _start

