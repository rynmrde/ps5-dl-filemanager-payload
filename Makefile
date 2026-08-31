# ps5-dl-filemanager-payload - unified PS5 ELF payload
# Requires ps5-payload-sdk (https://github.com/ps5-payload-dev/sdk)
# Set PS5_PAYLOAD_SDK to the SDK root, or use the provided Docker image.

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is not set. Install ps5-payload-sdk or use ghcr.io/ps5-payload-dev/sdk)
endif

CC        := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
CXX       := $(PS5_PAYLOAD_SDK)/bin/prospero-clang++
LD        := $(PS5_PAYLOAD_SDK)/bin/prospero-lld

TARGET    := payload.elf

CFLAGS    := -O2 -fPIC -funwind-tables -Wall -Wextra -Wshadow -fno-strict-aliasing \
             -march=znver2 -mtune=znver2 \
             -Iinclude
CXXFLAGS  := $(CFLAGS) -std=c++17 -fno-exceptions -fno-rtti
CFLAGS    += -std=c11 -isystem $(PS5_PAYLOAD_SDK)/target/include \
             -isystem $(PS5_PAYLOAD_SDK)/target/include_common
CXXFLAGS  += -idirafter $(PS5_PAYLOAD_SDK)/target/include \
             -idirafter $(PS5_PAYLOAD_SDK)/target/include_common
LDFLAGS   := -L$(PS5_PAYLOAD_SDK)/target/lib \
             -lc++ -lc++abi -lunwind -lpthread -lc

SRC_C     := src/main.c src/downloader.c src/filemanager.c src/ffpfsc.c
SRC_CXX   := src/unrar_engine.cpp
OBJ       := $(SRC_C:.c=.o) $(SRC_CXX:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDFLAGS)
	@echo "Built $(TARGET)"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
