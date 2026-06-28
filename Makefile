# Simple fallback build (no CMake required).
#   make            -> build ./side-hustle
#   make run        -> build and play
#   make selftest   -> build and run the headless smoke test
#   make clean      -> remove build artifacts

CXX      ?= g++
CXXFLAGS ?= -std=c++23 -O2 -Wall -Wextra -Isrc
SRC      := $(shell find src -name '*.cpp')
OBJ      := $(SRC:.cpp=.o)
BIN      := side-hustle

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN)

selftest: $(BIN)
	./$(BIN) --selftest --seed=1

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all run selftest clean
