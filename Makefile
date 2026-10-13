# Branchway NTFS: engine (Engine/), CLI tools (Tools/), static libntfs-3g (build/libntfs-3g).
#   make libntfs            build the pinned static libntfs-3g into build/libntfs-3g (never committed)
#   make                    ntfsharness (bench, fsck, journal tools) + bwntfs-format
#   make LIBNTFS=/Volumes/Media/spikes/ntfs/src/prefix-upstream    reuse the spike's prefix
#   make LIBNTFS=/opt/homebrew/opt/ntfs-3g-mac VERSION=2022.10.3    Homebrew variant (spike only)
NTFS3G_SRC ?= /Volumes/Media/spikes/ntfs/src/upstream
LIBNTFS ?= $(CURDIR)/build/libntfs-3g
VERSION ?= 2026.9.28-7f0f841f
BUILD   ?= build
CORE    := Engine
SRCS    := $(wildcard $(CORE)/*.c)
OBJS    := $(patsubst $(CORE)/%.c,$(BUILD)/%.o,$(SRCS))
CFLAGS  := -O2 -g -Wall -Wno-unused-parameter -arch arm64 -mmacosx-version-min=14.0 \
           -I$(CORE)/include -I$(CORE) -I$(LIBNTFS)/include -I$(LIBNTFS)/include/ntfs-3g \
           -DNH_LIBNTFS_VERSION='"$(VERSION)"'
LIBS    := $(LIBNTFS)/lib/libntfs-3g.a -framework CoreFoundation -lz

all: $(BUILD)/ntfsharness $(BUILD)/bwntfs-format

libntfs:
	Scripts/build-libntfs-3g.sh $(NTFS3G_SRC) $(CURDIR)/build/libntfs-3g-obj $(CURDIR)/build/libntfs-3g

$(BUILD)/%.o: $(CORE)/%.c $(CORE)/nh_internal.h $(CORE)/nh_config.h $(CORE)/include/bwntfs.h | $(BUILD)
	clang $(CFLAGS) -c $< -o $@

$(BUILD)/ntfsharness: Tools/ntfsharness/main.swift $(OBJS)
	swiftc -O -target arm64-apple-macos14.0 -import-objc-header $(CORE)/include/nhcore.h \
	  Tools/ntfsharness/main.swift $(OBJS) $(LIBS) -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -f $(BUILD)/*.o $(BUILD)/ntfsharness $(BUILD)/bwntfs-format $(BUILD)/libbwntfs.a $(BUILD)/bw_api_test

.PHONY: all clean libntfs

# the engine as a static library for the Xcode targets (Module/, the module tests); Xcode's
# pre-build step runs `make engine`, then links build/libbwntfs.a + build/libntfs-3g/lib/libntfs-3g.a
ENGINE_OBJS := $(OBJS)
$(BUILD)/libbwntfs.a: $(ENGINE_OBJS)
	rm -f $@ && libtool -static -o $@ $(ENGINE_OBJS) 2>/dev/null
engine: $(BUILD)/libbwntfs.a
.PHONY: engine

# engine API test over image files (Scripts/bw_api_test.py makes the image and runs it)
$(BUILD)/bw_api_test: Tests/engine/bw_api_test.c $(filter-out $(BUILD)/main.o,$(OBJS))
	clang $(CFLAGS) Tests/engine/bw_api_test.c $(OBJS) $(LIBS) -o $@

test-api: $(BUILD)/bw_api_test $(BUILD)/ntfsharness
	python3 Scripts/bw_api_test.py
.PHONY: test-api

# format CLI: mkntfs -Q -L + journal init on ONE named partition or image (Tools/bwntfs-format/main.c)
$(BUILD)/bwntfs-format: Tools/bwntfs-format/main.c $(filter-out $(BUILD)/main.o,$(OBJS))
	clang $(CFLAGS) Tools/bwntfs-format/main.c $(OBJS) $(LIBS) -framework DiskArbitration -framework IOKit -o $@

test-format: $(BUILD)/bwntfs-format $(BUILD)/ntfsharness
	python3 Tests/format/bwntfs_format_test.py
.PHONY: test-format
