# Host dashboard emulator. Build from the repository root with:
#   make -C emulator
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
BUILD := $(ROOT)/emulator/build
TARGET := $(BUILD)/smalltv-emulator
CXX ?= c++
ARDUINOJSON := $(firstword $(wildcard $(ROOT)/.pio/libdeps/*/ArduinoJson/src))
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null)
SECRET_CFLAGS := $(shell pkg-config --cflags libsecret-1 2>/dev/null)
SECRET_LIBS := $(shell pkg-config --libs libsecret-1 2>/dev/null)

ifeq ($(ARDUINOJSON),)
$(error ArduinoJson not found under .pio/libdeps; build the firmware once first)
endif
ifeq ($(SDL_LIBS),)
$(error SDL2 development files not found; install pkg-config and SDL2)
endif

CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wextra
CPPFLAGS += -DSMALLTV_ESP8266 -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 \
            -DARDUINOJSON_USE_DOUBLE=0 \
            -I$(ROOT)/emulator/shim -I$(ROOT)/tests/shim \
            -I$(ROOT)/src -I$(ROOT)/src/features/view \
            -I$(ROOT)/src/features/usage -I$(ROOT)/src/fonts \
            -I$(ROOT)/emulator/third_party -I$(ROOT)/emulator/third_party/imgui \
            -I$(ROOT)/emulator/third_party/imgui/backends \
            -I$(ARDUINOJSON) $(SDL_CFLAGS) $(CURL_CFLAGS) $(SECRET_CFLAGS)
LDLIBS += $(SDL_LIBS) -pthread
ifeq ($(OS),Windows_NT)
LDLIBS += -lws2_32 -liphlpapi -luser32 -mwindows -lwinhttp -lcrypt32
else
LDLIBS += $(CURL_LIBS) $(SECRET_LIBS)
endif

SRCS := $(ROOT)/emulator/main.cpp \
        $(ROOT)/emulator/theme.cpp \
        $(ROOT)/emulator/gfx.cpp \
        $(ROOT)/emulator/manager.cpp \
        $(ROOT)/emulator/usage/UsageHttp.cpp \
        $(ROOT)/emulator/usage/SecureStore.cpp \
        $(ROOT)/emulator/usage/UsageCore.cpp \
        $(ROOT)/emulator/usage/DeepSeekProxy.cpp \
        $(ROOT)/emulator/usage/Worker.cpp \
        $(ROOT)/src/features/view/ViewModel.cpp \
        $(ROOT)/src/features/view/ViewRender.cpp \
        $(ROOT)/src/features/view/ViewIcons.cpp \
        $(ROOT)/src/features/usage/UsageClient.cpp \
        $(ROOT)/src/fonts/NotoSansMonoAA.cpp \
        $(ROOT)/emulator/third_party/imgui/imgui.cpp \
        $(ROOT)/emulator/third_party/imgui/imgui_draw.cpp \
        $(ROOT)/emulator/third_party/imgui/imgui_tables.cpp \
        $(ROOT)/emulator/third_party/imgui/imgui_widgets.cpp \
        $(ROOT)/emulator/third_party/imgui/backends/imgui_impl_sdl2.cpp \
        $(ROOT)/emulator/third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp
OBJS := $(patsubst $(ROOT)/%.cpp,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(OBJS) $(LDLIBS) -o $@

$(BUILD)/%.o: $(ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(DEPS)

.PHONY: all clean
