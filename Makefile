# ps5-dl-filemanager-payload - unified PS5 ELF payload
# Requires ps5-payload-sdk (https://github.com/ps5-payload-dev/sdk)
# Set PS5_PAYLOAD_SDK to the SDK root, or use the provided Docker image.

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is not set. Install ps5-payload-sdk or use ghcr.io/ps5-payload-dev/sdk)
endif

TOOLCHAIN := $(PS5_PAYLOAD_SDK)/toolchain/prospero
CC        := $(TOOLCHAIN)/bin/clang
CXX       := $(TOOLCHAIN)/bin/clang++
LD        := $(TOOLCHAIN)/bin/ld.lld

TARGET    := payload.elf

CFLAGS    := -O2 -fPIC -funwind-tables -Wall -Wextra -Wshadow -fno-strict-aliasing \
             -target x86_64-pc-freebsd12-elf -march=znver2 -mtune=znver2 \
             -Iinclude -I$(PS5_PAYLOAD_SDK)/include
CXXFLAGS  := $(CFLAGS) -std=c++17 -fno-exceptions -fno-rtti
CFLAGS    += -std=c11
LDFLAGS   := -nostdlib -L$(PS5_PAYLOAD_SDK)/lib \
             -lkernel_sys -lc -lSceLibcInternal -lunrar -lpthread -lz

SRC_C     := src/main.c src/downloader.c src/filemanager.c src/ffpfsc.c
SRC_CXX   := src/unrar_engine.cpp
OBJ       := $(SRC_C:.c=.o) $(SRC_CXX:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)
	@echo "Built $(TARGET)"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
