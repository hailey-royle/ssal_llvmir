COMPILER = clang
SHELL := /bin/bash
# t := test_name

build:
	$(COMPILER) ssal_llvmir.c -o ssal_llvmir -Wall -Wextra -std=c89 -O3

debug:
	$(COMPILER) ssal_llvmir.c -o ssal_llvmir -Wall -Wextra -std=c89 -Og -ggdb

test:
ifdef t
	./ssal_llvmir test/$(t)/test.ssal
	clang test/$(t)/test.ssal.ll -o $(t)
else
	$(SHELL) -c "cd test && ./test.sh"
endif

.PHONY: build debug test clean

