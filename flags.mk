# Single source of the build flags.
#
# Read by the Makefile (via include) and by build.bat (via for /f delims==).
# build.bat imposes the format: one variable per line, no spaces around '=', and
# no make-specific syntax. Keeping both readers on this one file is what stops
# the two build paths from drifting apart.

# -fstack-protector-strong: the program parses bytes that originate outside the
# computer. EDID and the MCCS capability string arrive over I2C from monitor
# firmware, and a dock, KVM or USB-C monitor controls that content. The parsers
# are bounded, but this is the code path where a future mistake would become code
# execution, and the canary is nearly free.
#
# The linker hardening flags are explicit rather than inherited: DEP and ASLR
# happen to default on in this binutils version, and relying on one version's
# default is not a guarantee. Control Flow Guard is unavailable here because
# GCC/MinGW does not implement it; it would require MSVC or clang-cl.
ZDISPLAY_STD=-std=c++17 -municode -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN
ZDISPLAY_WARN=-Wall -Wextra -Wformat=2 -Wformat-security -Werror=format-security -Wno-unused-parameter -Wno-cast-function-type -Wno-missing-field-initializers
ZDISPLAY_REL=-O2 -DNDEBUG -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fstack-protector-strong
ZDISPLAY_DBG=-g -O0 -DDEBUG -fstack-protector-strong -D_GLIBCXX_ASSERTIONS
ZDISPLAY_LD=-municode -mwindows -static -static-libgcc -static-libstdc++ -fstack-protector-strong -Wl,--nxcompat -Wl,--dynamicbase -Wl,--high-entropy-va -Wl,--no-insert-timestamp
ZDISPLAY_LDREL=-Wl,--gc-sections -s
ZDISPLAY_LIBS=-lgdi32 -luser32 -ladvapi32 -lshell32 -lcomctl32 -lcomdlg32 -lole32 -loleaut32 -lshlwapi -lpsapi -luuid

# Tests: pure logic, no hardware dependency. backends_display.cpp is included
# because BlendRamp, ComposeWithBaseline, RampIsIdentity and BuildMatrix are pure
# and nothing runs at load time.
#
# -D_GLIBCXX_ASSERTIONS enables libstdc++ bounds checking (vector and string
# operator[], iterators). It applies to tests and debug builds, not to release: a
# failed check aborts, and aborting in release would leave the user's display
# stuck on whatever adjustment was active. In the tests aborting is the desired
# outcome, since it turns every case into a bounds check.
ZDISPLAY_TESTFLAGS=-O1 -D_GLIBCXX_ASSERTIONS -fstack-protector-strong
ZDISPLAY_TESTSRC=tests/test_zdisplay.cpp src/core.cpp src/common.cpp src/backends_display.cpp
ZDISPLAY_TESTLIBS=-lshell32 -lole32 -ladvapi32 -luser32 -lgdi32 -luuid

# Portable toolchain that build.bat downloads when no compiler is present.
# The script verifies both the Authenticode signature and this SHA-256 before
# extracting anything.
ZDISPLAY_TOOLCHAIN_URL=https://github.com/skeeto/w64devkit/releases/download/v2.9.0/w64devkit-x64-2.9.0.7z.exe
ZDISPLAY_TOOLCHAIN_SHA256=BFF1D13FC2718EEBD93548CF37F8D0332D925458D5E99506CFF8F46EB5A9DE5A
