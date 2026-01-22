COMMON_SRCS = src/utils.c src/des.c src/netntlmv1.c src/rainbow.c src/table.c src/opencl_host.c src/opencl_dyn.c src/platform.c
LOOKUP_SRCS = src/main.c $(COMMON_SRCS)
PRECOMPUTE_SRCS = src/precompute_main.c $(COMMON_SRCS)
CANDIDATE_LOOKUP_SRCS = src/candidate_lookup_main.c $(COMMON_SRCS)
CANDIDATE_CHECK_SRCS = src/candidate_check_main.c $(COMMON_SRCS)

ifeq ($(OS),Windows_NT)
    ifndef VSCMD_ARG_TGT_ARCH
        $(warning WARNING: Not in a Visual Studio Developer Command Prompt)
    else ifneq ($(VSCMD_ARG_TGT_ARCH),x64)
        $(error ERROR: Use "x64 Native Tools Command Prompt for VS 2022")
    endif

    CC = cl
    CFLAGS = /O2 /W3 /Iinclude /Idep /nologo /D_CRT_SECURE_NO_WARNINGS
    LDFLAGS =
    RM = del /f /q

all: gpu_lookup.exe precompute.exe candidate_lookup.exe candidate_check.exe

gpu_lookup.exe: $(LOOKUP_SRCS)
	$(CC) $(CFLAGS) $(LOOKUP_SRCS) /Fe:$@ $(LDFLAGS)

precompute.exe: $(PRECOMPUTE_SRCS)
	$(CC) $(CFLAGS) $(PRECOMPUTE_SRCS) /Fe:$@ $(LDFLAGS)

candidate_lookup.exe: $(CANDIDATE_LOOKUP_SRCS)
	$(CC) $(CFLAGS) $(CANDIDATE_LOOKUP_SRCS) /Fe:$@ $(LDFLAGS)

candidate_check.exe: $(CANDIDATE_CHECK_SRCS)
	$(CC) $(CFLAGS) $(CANDIDATE_CHECK_SRCS) /Fe:$@ $(LDFLAGS)

clean:
	-$(RM) *.exe *.obj 2>nul

else

CC = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -O2 -Iinclude -Idep
LDFLAGS = -ldl
RM = rm -f
MINGW = x86_64-w64-mingw32-gcc
MINGW_FLAGS = -Wall -Wextra -std=gnu99 -O2 -Iinclude -Idep -Wno-cast-function-type -static

all: gpu_lookup precompute candidate_lookup candidate_check

gpu_lookup: $(LOOKUP_SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

precompute: $(PRECOMPUTE_SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

candidate_lookup: $(CANDIDATE_LOOKUP_SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

candidate_check: $(CANDIDATE_CHECK_SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

windows: gpu_lookup.exe precompute.exe candidate_lookup.exe candidate_check.exe

gpu_lookup.exe: $(LOOKUP_SRCS)
	$(MINGW) $(MINGW_FLAGS) $^ -o $@

precompute.exe: $(PRECOMPUTE_SRCS)
	$(MINGW) $(MINGW_FLAGS) $^ -o $@

candidate_lookup.exe: $(CANDIDATE_LOOKUP_SRCS)
	$(MINGW) $(MINGW_FLAGS) $^ -o $@

candidate_check.exe: $(CANDIDATE_CHECK_SRCS)
	$(MINGW) $(MINGW_FLAGS) $^ -o $@

clean:
	$(RM) gpu_lookup gpu_lookup.exe precompute precompute.exe candidate_lookup candidate_lookup.exe candidate_check candidate_check.exe

endif

.PHONY: all clean windows