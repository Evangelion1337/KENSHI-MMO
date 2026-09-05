CXX = x86_64-w64-mingw32-g++
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -shared -nostdlib++ -fno-exceptions -Isrc -Iserver
LDFLAGS_DLL = -shared -nostdlib++
LDFLAGS_EXE = -nostdlib++
LIBS_DLL = -lws2_32 -lcomctl32 -lgdi32 -lkernel32 -lshell32
LIBS_EXE = -lws2_32 -ladvapi32 -lkernel32

SRCS_DLL = src/plugin.cpp src/log.cpp src/mem.cpp src/scan.cpp src/discovery.cpp \
           src/sha256.cpp src/net.cpp src/panel.cpp src/session.cpp src/world.cpp src/savesync.cpp src/savemgr.cpp src/speedlock.cpp src/engine.cpp
OBJS_DLL = $(SRCS_DLL:.cpp=.o)
TARGET_DLL = KenshiMMO.dll

SRCS_EXE = server/server.cpp server/srvlog.cpp server/accounts.cpp src/sha256.cpp
OBJS_EXE = $(SRCS_EXE:.cpp=.o)
TARGET_EXE = KenshiMMO.Server.exe

all: $(TARGET_DLL) $(TARGET_EXE)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET_DLL): $(OBJS_DLL)
	$(CXX) $(LDFLAGS_DLL) $^ $(LIBS_DLL) -Wl,src/plugin.def -o $@

$(TARGET_EXE): $(OBJS_EXE)
	$(CXX) $(LDFLAGS_EXE) $^ $(LIBS_EXE) -o $@

clean:
	rm -f $(OBJS_DLL) $(OBJS_EXE) $(TARGET_DLL) $(TARGET_EXE)

.PHONY: all clean