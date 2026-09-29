/*
 * Copyright (c) 2026, Barcelona Supercomputing Center
 * Contact: mess             [at] bsc [dot] es
 *          victor.xirau     [at] bsc [dot] es
 *          mariana.carmin   [at] bsc [dot] es
 *          pau.diazcuesta   [at] bsc [dot] es
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

#include "utils/SubprocessCapture.h"

#include "platform/Process.h"

namespace subprocess {

namespace {

platform::RunOptions to_platform(const Options& opts) {
    platform::RunOptions out;
    out.timeout = opts.timeout;
    out.max_output_bytes = opts.max_output_bytes;
    return out;
}

Result from_platform(platform::RunResult&& r) {
    Result out;
    out.output = std::move(r.output);
    out.exit_status = r.exit_status;
    out.timed_out = r.timed_out;
    out.spawn_failed = r.spawn_failed;
    return out;
}

} // namespace

Result capture(const std::string& program,
               const std::vector<std::string>& args,
               const Options& opts) {
    return from_platform(platform::run(program, args, to_platform(opts)));
}

Result capture_shell(const std::string& command, const Options& opts) {
    return from_platform(platform::run_shell(command, to_platform(opts)));
}

std::string run_shell(const std::string& command, const Options& opts) {
    return capture_shell(command, opts).output;
}

std::string run(const std::string& program,
                const std::vector<std::string>& args,
                const Options& opts) {
    return capture(program, args, opts).output;
}

}
