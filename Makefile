# Build targets:
#   make            -> terminal game ./side-hustle
#   make run        -> build and play (terminal)
#   make selftest   -> headless logic smoke test
#   make windows    -> ./side-hustle.exe        (terminal, cross-compiled)
#
# Graphical (raylib) version -- run scripts/fetch-raylib.sh once first:
#   make gui         -> ./side-hustle-gui        (Linux window)
#   make windows-gui -> ./side-hustle-gui.exe    (Windows window, single file)
#
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

# Graphical front-end (raylib). Shared engine = all of src/ except the
# terminal entry point, plus the raylib main.
RAY_INC  := third_party/raylib/include
GUI_SRC  := gui/Main.cpp $(filter-out src/main.cpp,$(SRC))
# Background music (OGG) embedded via .incbin -> single self-contained binary.
GUI_MUSIC := gui/music_data.S
GUI_BIN  := side-hustle-gui
GUI_WIN  := side-hustle-gui.exe
LINUX_GL := -lraylib -lGL -lX11 -lXrandr -lXinerama -lXi -lXcursor -lm -lpthread -ldl -lrt
WIN_GL   := -lraylib -lopengl32 -lgdi32 -lwinmm

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# `make windows` -> ./side-hustle.exe (no DLLs needed beyond stock Windows).
windows: $(WINBIN)

$(WINBIN): $(SRC)
	$(WINCXX) $(WINFLAGS) -o $@ $(SRC)

gui:
	$(CXX) $(CXXFLAGS) -I$(RAY_INC) $(GUI_SRC) $(GUI_MUSIC) -Lthird_party/raylib/lib/linux $(LINUX_GL) -o $(GUI_BIN)

windows-gui:
	$(WINCXX) $(WINFLAGS) -I$(RAY_INC) $(GUI_SRC) $(GUI_MUSIC) -Lthird_party/raylib/lib/win64 $(WIN_GL) -mwindows -o $(GUI_WIN)

run: $(BIN)
	./$(BIN)

selftest: $(BIN)
	./$(BIN) --selftest --seed=1

clean:
	rm -f $(OBJ) $(BIN) $(WINBIN) $(GUI_BIN) $(GUI_WIN)

.PHONY: all windows gui windows-gui run selftest clean
