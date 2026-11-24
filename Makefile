# C++17, MSYS2 MinGW-w64 on Windows or GCC on Linux.
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
ifeq ($(OS),Windows_NT)
LDFLAGS ?= -static
EXEEXT := .exe
CLI_LDFLAGS ?= -municode
PYTHON ?= python
else
LDFLAGS ?= -static-libgcc -static-libstdc++
EXEEXT :=
CLI_LDFLAGS ?=
CXXFLAGS += -pthread -D_FILE_OFFSET_BITS=64
PYTHON ?= python3
endif
TARGET ?= nevtoraw$(EXEEXT)
TEST_TARGET ?= raw-input-test$(EXEEXT)
HE_PUB := third_party/nikon-he-public

SRCS := $(wildcard src/*.cpp)
PUB_SRCS := $(wildcard $(HE_PUB)/*.cpp)

all: $(TARGET)

$(TARGET): $(SRCS) $(PUB_SRCS) $(wildcard src/*.h) $(wildcard src/*.inc) $(wildcard $(HE_PUB)/*.h)
	$(CXX) $(CXXFLAGS) -I$(HE_PUB) $(SRCS) $(PUB_SRCS) -o $@ $(LDFLAGS) $(CLI_LDFLAGS)

clean:
	rm -f $(TARGET)

$(TEST_TARGET): tests/raw_input.cpp $(filter-out src/main.cpp,$(SRCS)) $(PUB_SRCS) $(wildcard src/*.h) $(wildcard src/*.inc)
	$(CXX) $(CXXFLAGS) -Isrc -I$(HE_PUB) tests/raw_input.cpp $(filter-out src/main.cpp,$(SRCS)) $(PUB_SRCS) -o $@ $(LDFLAGS)

test: $(TARGET) $(TEST_TARGET)
	./$(TEST_TARGET)
	$(PYTHON) tests/cli.py $(TARGET)

.PHONY: all clean test
