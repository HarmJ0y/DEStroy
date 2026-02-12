# Makefile for GPU Rainbow Table Sorter

NVCC = nvcc
NVCC_FLAGS = -O3 -std=c++17 -arch=sm_89
LDFLAGS = -lpthread

# For L4 GPUs (Ada Lovelace architecture)
# sm_89 is the correct compute capability

TARGET = rtsort_gpu
SRC = rtsort_gpu.cu

all: $(TARGET)

$(TARGET): $(SRC)
	$(NVCC) $(NVCC_FLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

# Debug build with symbols
debug: NVCC_FLAGS += -g -G
debug: $(TARGET)

# Profile build
profile: NVCC_FLAGS += -lineinfo
profile: $(TARGET)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

.PHONY: all debug profile clean install
