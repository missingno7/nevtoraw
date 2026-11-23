# Build with MSYS2 MinGW-w64 g++ (or any C++17 compiler):  mingw32-make
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LDFLAGS ?= -static
CLI_LDFLAGS ?= -municode
TARGET ?= nevtoraw.exe
PYTHON ?= python
HE_PUB := third_party/nikon-he-public

SRCS := $(wildcard src/*.cpp)
PUB_SRCS := $(wildcard $(HE_PUB)/*.cpp)

all: $(TARGET)

$(TARGET): $(SRCS) $(PUB_SRCS) $(wildcard src/*.h) $(wildcard src/*.inc) $(wildcard $(HE_PUB)/*.h)
	$(CXX) $(CXXFLAGS) -I$(HE_PUB) $(SRCS) $(PUB_SRCS) -o $@ $(LDFLAGS) $(CLI_LDFLAGS)

clean:
	rm -f $(TARGET)

raw-input-test.exe: tests/raw_input.cpp $(filter-out src/main.cpp,$(SRCS)) $(PUB_SRCS) $(wildcard src/*.h) $(wildcard src/*.inc)
	$(CXX) $(CXXFLAGS) -Isrc -I$(HE_PUB) tests/raw_input.cpp $(filter-out src/main.cpp,$(SRCS)) $(PUB_SRCS) -o $@ $(LDFLAGS)

test: $(TARGET) raw-input-test.exe
	./raw-input-test.exe
	$(PYTHON) tests/cli.py $(TARGET)

.PHONY: all clean test
