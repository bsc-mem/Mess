/*
 * Copyright (c) 2026, Barcelona Supercomputing Center
 * Contact: mess             [at] bsc [dot] es
 *          victor.xirau     [at] bsc [dot] es
 *          petar.radojkovic [at] bsc [dot] es
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 *     * Neither the name of the copyright holder nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef PLATFORM_CONSOLE_H
#define PLATFORM_CONSOLE_H

#include <string>

namespace platform {

void init_console();
bool stdout_is_tty();
void hide_cursor();
void show_cursor();
void show_cursor_signal_safe();
void install_interrupt_handler(void (*fn)(int));

inline const std::string& clear_line() {
    static const std::string tty_clear = "\r\033[K";
    static const std::string pipe_clear = "\r" + std::string(140, ' ') + "\r";
    return stdout_is_tty() ? tty_clear : pipe_clear;
}

namespace glyph {
#if defined(_WIN32)
constexpr const char* kWarning = "\xE2\x96\xB2";
constexpr const char* kOk      = "\xE2\x88\x9A";
constexpr const char* kFail    = "\xC3\x97";
constexpr const char* kRetry   = "\xC2\xBB";
#else
constexpr const char* kWarning = "\xE2\x9A\xA0";
constexpr const char* kOk      = "\xE2\x9C\x93";
constexpr const char* kFail    = "\xE2\x9C\x97";
constexpr const char* kRetry   = "\xE2\x86\xBA";
#endif
}

}

#endif // PLATFORM_CONSOLE_H
