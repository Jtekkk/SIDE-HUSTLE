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

# Cross-compile a self-contained Windows .exe with mingw-w64.
WINCXX   ?= x86_64-w64-mingw32-g++
WINFLAGS ?= -std=c++23 -O2 -Wall -Wextra -Isrc -static -static-libgcc -static-libstdc++
WINBIN   := side-hustle.exe

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# `make windows` -> ./side-hustle.exe (no DLLs needed beyond stock Windows).
windows: $(WINBIN)

$(WINBIN): $(SRC)
	$(WINCXX) $(WINFLAGS) -o $@ $(SRC)

run: $(BIN)
	./$(BIN)

selftest: $(BIN)
	./$(BIN) --selftest --seed=1

clean:
	rm -f $(OBJ) $(BIN) $(WINBIN)

.PHONY: all windows run selftest clean
