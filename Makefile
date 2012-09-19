SRCS = libsheriff.cpp realfuncs.cpp xthread.cpp dlmalloc.c finetime.c gnuwrapper.cpp 
DEPS = $(SRCS) xpersist.h xdefines.h xglobals.h xpersist.h xplock.h xrun.h warpheap.h xadaptheap.h xoneheap.h

CXX = g++

# Detection on 32bit
# CXX = g++ -DSSE_SUPPORT -m32 -DX86_32BIT -O3 -fno-omit-frame-pointer -DDETECT_FALSE_SHARING
# Detection on 64bit
#CXX = g++ -DSSE_SUPPORT -m64 -fno-omit-frame-pointer -DDETECT_FALSE_SHARING


# -march=core2 -msse3 -DSSE_SUPPORT 
CFLAGS   = -msse3 -DSSE_SUPPORT -fno-omit-frame-pointer
CFLAGS32 = $(CFLAGS) -m32 -DX86_32BIT -O3
CFLAGS64 = $(CFLAGS) -m64 -O3

INCLUDE_DIRS = -I. -I./heaplayers -I./heaplayers/util

#GET_CHARACTERISTICS

TARGETS = libsheriff_protect32.so libsheriff_detect32.so libsheriff_protect64.so libsheriff_detect64.so

all: $(TARGETS)

libsheriff_protect32.so: $(DEPS)
	$(CXX) $(CFLAGS32) $(INCLUDE_DIRS) -shared -fPIC -D'CUSTOM_PREFIX(x)=sheriff_##x' $(SRCS) -o libsheriff_protect32.so  -ldl -lpthread

libsheriff_detect32.so: $(DEPS)
	$(CXX) -DDETECT_FALSE_SHARING $(CFLAGS32) $(INCLUDE_DIRS) -shared -fPIC -D'CUSTOM_PREFIX(x)=sheriff_##x'  $(SRCS) -o libsheriff_detect32.so  -ldl -lpthread

libsheriff_protect64.so: $(DEPS)
	$(CXX) $(CFLAGS64) $(INCLUDE_DIRS) -shared -fPIC -D'CUSTOM_PREFIX(x)=sheriff_##x' $(SRCS) -o libsheriff_protect64.so  -ldl -lpthread

libsheriff_detect64.so: $(DEPS)
	$(CXX) -DDETECT_FALSE_SHARING $(CFLAGS64) $(INCLUDE_DIRS) -shared -fPIC -D'CUSTOM_PREFIX(x)=sheriff_##x'  $(SRCS) -o libsheriff_detect64.so  -ldl -lpthread

clean:
	rm -f $(TARGETS)

