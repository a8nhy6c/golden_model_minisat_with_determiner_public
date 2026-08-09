# Makefile for the minisat grounded golden model.
# minisat is C++98 era code; clang in c++11 mode rejects its "%4"PRIi64 literals, so the whole
# project builds with gnu++98. Solver.cc and System.cc are compiled from the bundled minisat/ copy.

CXX ?= c++
MINISAT ?= minisat
CXXFLAGS ?= -std=gnu++98 -O2 -Wall -I$(MINISAT) -D__STDC_LIMIT_MACROS -D__STDC_FORMAT_MACROS

all: examples/expanded_all_ops examples/determiner_ops examples/fullchip_ops

Solver.o: $(MINISAT)/minisat/core/Solver.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

System.o: $(MINISAT)/minisat/utils/System.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

examples/%: examples/%.cpp golden_model_core.cpp Solver.o System.o
	$(CXX) $(CXXFLAGS) $< Solver.o System.o -o $@

random_generator/%: random_generator/%.cpp golden_model_core.cpp Solver.o System.o
	$(CXX) $(CXXFLAGS) $< Solver.o System.o -o $@

clean:
	rm -f *.o examples/expanded_all_ops examples/out_results.txt examples/out_checked.vec
	rm -f examples/determiner_ops examples/out_determiner_results.txt examples/out_determiner.vec
	rm -f examples/fullchip_ops examples/out_fullchip_results.txt examples/out_fullchip.vec
	rm -f examples/template_ops examples/out_template_results.txt examples/out_template.vec
	rm -f random_generator/random_vec_generator random_generator/out_random_*.vec random_generator/out_random_*_results.txt

.PHONY: all clean
