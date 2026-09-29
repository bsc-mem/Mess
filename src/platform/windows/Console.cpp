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

#include "platform/Console.h"

#include <windows.h>

#include <csignal>
#include <iostream>

namespace platform {

namespace {

constexpr char kHide[] = "\033[?25l";
constexpr char kShow[] = "\033[?25h";

void (*g_interrupt_fn)(int) = nullptr;

// Runs on an OS-created thread. CTRL_CLOSE gives about 5 seconds before the process dies, so
// this must not block.
BOOL WINAPI console_ctrl_routine(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_interrupt_fn) {
                g_interrupt_fn(SIGINT);
            }
            return TRUE;
        default:
            return FALSE;
    }
}

bool add_console_mode(DWORD std_handle, DWORD flags) {
    const HANDLE h = GetStdHandle(std_handle);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;

    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) {
        return false;
    }
    return SetConsoleMode(h, mode | flags) != 0;
}

}

void init_console() {
    // Without ENABLE_VIRTUAL_TERMINAL_PROCESSING escape sequences print literally. Per handle,
    // so stderr needs it too.
    add_console_mode(STD_OUTPUT_HANDLE, ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    add_console_mode(STD_ERROR_HANDLE, ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    SetConsoleOutputCP(CP_UTF8);
}

bool stdout_is_tty() {
    const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;

    // _isatty() is not equivalent: it also returns true for a character device such as NUL.
    DWORD mode = 0;
    return GetConsoleMode(h, &mode) != 0;
}

void hide_cursor() {
    if (stdout_is_tty()) {
        std::cout << kHide << std::flush;
    }
}

void show_cursor() {
    if (stdout_is_tty()) {
        std::cout << kShow << std::flush;
    }
}

void show_cursor_signal_safe() {
    show_cursor();
}

void install_interrupt_handler(void (*fn)(int)) {
    g_interrupt_fn = fn;
    SetConsoleCtrlHandler(console_ctrl_routine, TRUE);

    std::signal(SIGINT, fn);
    std::signal(SIGTERM, fn);
}

}
