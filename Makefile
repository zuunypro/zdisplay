# Zdisplay — build with make + MinGW-w64.
# Alternative to build.bat when the toolchain is already on PATH.
#
#   make            release build
#   make debug      build with symbols
#   make check      build the application AND run the test suite
#   make test       test suite only
#   make icon       generate assets/zdisplay.ico using the program itself
#   make clean

CXX      ?= g++
WINDRES  ?= windres

include flags.mk

# Shell selection on Windows.
#
# With only MinGW-w64 on PATH there is no POSIX shell: make still defaults to
# SHELL=sh.exe and then invokes each command through CreateProcess, where `cp`
# and `mkdir -p` do not exist, because MinGW-w64 alone ships no coreutils. The
# recipes then have to go through cmd and use only its built-ins.
#
# Under MSYS2, Cygwin or Git Bash a real POSIX shell is present and must be used
# instead: cmd treats the forward slashes in `-c src/common.cpp` as switches and
# mangles the command line. MSYSTEM is set by all three environments.
ifeq ($(OS)$(MSYSTEM),Windows_NT)
  SHELL := $(COMSPEC)
  .SHELLFLAGS := /C
  EXEPATH = $(subst /,\,$(1))
  MKDIR = if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"
  TOUCH = if not exist "$(subst /,\,$(1))" type nul > "$(subst /,\,$(1))"
  COPY  = copy /y "$(subst /,\,$(1))" "$(subst /,\,$(2))" >nul
  RMRF  = if exist "$(subst /,\,$(1))" rmdir /s /q "$(subst /,\,$(1))"
  DEL   = if exist "$(subst /,\,$(1))" del /q "$(subst /,\,$(1))"
else
  EXEPATH = ./$(1)
  MKDIR = mkdir -p "$(1)"
  TOUCH = touch "$(1)"
  COPY  = cp "$(1)" "$(2)"
  RMRF  = rm -rf $(1)
  DEL   = rm -f $(1)
endif

SRC := src/common.cpp src/core.cpp src/icon.cpp \
       src/backends_display.cpp src/backends_vendor.cpp src/backends_hw.cpp \
       src/engine.cpp src/services.cpp \
       src/ui_app.cpp src/ui_settings.cpp src/ui_events.cpp src/ui_guard.cpp \
       src/ui_theme.cpp src/main.cpp

# Release and debug use different flags and therefore need separate object
# directories: a shared one lets one configuration reuse the other's objects and
# link a binary with inconsistent struct layouts.
ifneq (,$(filter debug,$(MAKECMDGOALS)))
  CONFIG   := debug
  OPTFLAGS := $(ZDISPLAY_DBG)
  LDEXTRA  :=
else
  CONFIG   := release
  OPTFLAGS := $(ZDISPLAY_REL)
  LDEXTRA  := $(ZDISPLAY_LDREL)
endif

BUILDDIR := build/$(CONFIG)
OBJ      := $(patsubst src/%.cpp,$(BUILDDIR)/%.o,$(SRC))
DEPS     := $(OBJ:.o=.d)

# -MMD -MP generates header dependencies, so editing a header triggers a rebuild.
CXXFLAGS := $(ZDISPLAY_STD) $(ZDISPLAY_WARN) $(OPTFLAGS) -MMD -MP
LDFLAGS  := $(ZDISPLAY_LD) $(LDEXTRA)
LIBS     := $(ZDISPLAY_LIBS)

# The icon is embedded only once it exists on disk; see the "icon" target.
RESFLAGS :=
ifneq ($(wildcard assets/zdisplay.ico),)
  RESFLAGS += -DZDISPLAY_HAS_ICON
endif

.PHONY: all debug check clean icon test
all: zdisplay.exe
debug: zdisplay.exe

# Builds the application and runs the suite in one invocation. `all` does not
# depend on `test`, so this is the target that exercises both the link and the
# tests together.
check: $(BUILDDIR)/zdisplay.exe test

# Pure-logic tests: no hardware and no monitor required.
#
# The test binary gets a directory of its own containing the portable-mode
# marker. ConfigDir() follows the executable's directory, so this isolation keeps
# the suite from sharing zdisplay.ini, baseline.dat and session.lock with a
# running instance, and from overwriting the configuration of a portable install.
test: | $(BUILDDIR)
	@$(call MKDIR,$(BUILDDIR)/test)
	@$(call TOUCH,$(BUILDDIR)/test/zdisplay-portable.txt)
	$(CXX) $(ZDISPLAY_STD) $(ZDISPLAY_WARN) $(ZDISPLAY_TESTFLAGS) $(ZDISPLAY_TESTSRC) \
	  -o $(BUILDDIR)/test/test_zdisplay.exe $(ZDISPLAY_TESTLIBS)
	$(call EXEPATH,$(BUILDDIR)/test/test_zdisplay.exe)

# The link produces the binary inside build/. Copying it to the project root is a
# separate step, so a build can be verified without overwriting a running
# zdisplay.exe.
$(BUILDDIR)/zdisplay.exe: $(OBJ) $(BUILDDIR)/zdisplay_res.o
	$(CXX) $(OBJ) $(BUILDDIR)/zdisplay_res.o -o $@ $(LDFLAGS) $(LIBS)

# The copy fails while a running zdisplay.exe holds the file. That is not a build
# error — the new binary is complete inside build/ — so it is reported as a
# warning.
zdisplay.exe: $(BUILDDIR)/zdisplay.exe
	@$(call COPY,$<,$@) || echo AVISO: zdisplay.exe em uso; o binario novo esta em $(call EXEPATH,$<)
	@echo Pronto.

$(BUILDDIR)/%.o: src/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/zdisplay_res.o: src/zdisplay.rc src/app.manifest src/version.h | $(BUILDDIR)
	$(WINDRES) $(RESFLAGS) -i src/zdisplay.rc -o $@

$(BUILDDIR):
	@$(call MKDIR,$(BUILDDIR))

# Generates the icon using the binary itself, then rebuilds to embed it.
icon: $(BUILDDIR)/zdisplay.exe
	@$(call MKDIR,assets)
	$(call EXEPATH,$(BUILDDIR)/zdisplay.exe) --make-icon
	$(MAKE) -B $(BUILDDIR)/zdisplay_res.o $(BUILDDIR)/zdisplay.exe

clean:
	-@$(call RMRF,build)
	-@$(call DEL,zdisplay.exe)

-include $(DEPS)
