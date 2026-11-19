# Build with MSYS2 MinGW-w64 g++ (or any C++17 compiler):  mingw32-make
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LDFLAGS ?= -static
HE_PUB := third_party/nikon-he-public

SRCS := src/main.cpp src/nev.cpp src/meta.cpp src/dng.cpp src/tiffw.cpp src/lj92.cpp src/he_decode.cpp src/stack.cpp
PUB_SRCS := $(wildcard $(HE_PUB)/*.cpp)

all: nevtoraw.exe

nevtoraw.exe: $(SRCS) $(PUB_SRCS) $(wildcard src/*.h) $(wildcard $(HE_PUB)/*.h)
	$(CXX) $(CXXFLAGS) -I$(HE_PUB) $(SRCS) $(PUB_SRCS) -o $@ $(LDFLAGS)

clean:
	rm -f nevtoraw.exe

.PHONY: all clean
