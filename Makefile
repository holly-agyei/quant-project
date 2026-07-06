# Lock-Free Pre-Trade Risk Gate — build/test/bench
CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -pthread -Wall -Wextra

.PHONY: all test bench clean

all: test bench

test: test.cpp risk_gate.hpp
	$(CXX) $(CXXFLAGS) test.cpp -o test

bench: bench.cpp risk_gate.hpp
	$(CXX) $(CXXFLAGS) bench.cpp -o bench

# Build and run the correctness + concurrency suite.
run-test: test
	./test

# Build and run the latency/throughput benchmark.
run-bench: bench
	./bench

clean:
	rm -f test bench
