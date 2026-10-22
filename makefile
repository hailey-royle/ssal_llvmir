SHELL := /bin/bash
# t := test_name

build:
	clang ssal_llvmir.c -o ssal_llvmir -Wall -Wextra -std=c89 -O3

debug:
	clang ssal_llvmir.c -o ssal_llvmir -Wall -Wextra -std=c89 -Og -ggdb

test: debug
ifdef t
	cat test/$(t)/test.ssal
	./ssal_llvmir test/$(t)/test.ssal
	cat test/$(t)/test.ll
	clang test/$(t)/test.ll _start.s -o $(t) -masm=intel -static -nostdlib
else
	$(SHELL) -c "cd test && ./test.sh"
endif

.PHONY: build debug test clean _start

