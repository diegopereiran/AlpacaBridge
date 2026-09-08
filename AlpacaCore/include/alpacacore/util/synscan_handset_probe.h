// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

// A SynScan hand controller exposes the same Prolific / FTDI / CP210x adapter
// classes that several other serial auto-detect scans target (Sky-Watcher
// motor controllers, iOptron focusers and filter wheels), and it speaks its
// own protocol at 9600 8N1. A SynScan V4 handset (firmware 04.40.00, built-in
// PL2303 067b:23a3) stops answering serial ENTIRELY after it receives bytes at
// the wrong rate - one motor-controller probe at 115200 is enough - and only
// a power-cycle brings it back. Seen on the EQM-35 Pro rig in 2026-09: the
// handset was silent to every protocol at every baud until rebooted, and went
// silent again the moment 115200 traffic touched it, which is what the
// "hand-controller commands time out" report actually was.
//
// Every scan that may send non-9600 traffic to a Prolific-class port should
// therefore ask for the handset's echo first and leave the port alone when it
// answers. The echo ("K" + byte -> byte + "#") is the protocol's own link
// check, costs ~20 ms on a live handset, and is harmless to everything else.
//
// POSIX-only, like the serial scans it serves.

#ifndef _WIN32

#include <alpacacore/util/serial_io.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <string>

namespace alpacacore::util {

/**
 * @brief True if a SynScan hand controller answers its protocol echo on
 *        @p port_path at 9600 8N1 within @p timeout_ms.
 *
 * Strict: only the exact echo (the sent byte followed by '#') counts, so a
 * port that answers something else is not mistaken for a handset. A silent
 * port returns false after the timeout - a wedged handset therefore looks
 * like no handset, which is the honest answer (it will not respond to
 * anything until it is power-cycled).
 */
inline bool port_answers_synscan_echo(const std::string& port_path, int timeout_ms = 500) {
    const int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    struct termios tty{};
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return false;
    }
    cfmakeraw(&tty);
    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag &= ~HUPCL;  // do not drop DTR on close: some adapters reset the far end on it
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;  // 100 ms per read, the deadline below bounds the loop
    if (tcsetattr(fd, TCSANOW, &tty) != 0 || !clear_nonblocking(fd)) {
        close(fd);
        return false;
    }
    tcflush(fd, TCIOFLUSH);

    constexpr char kEchoByte = 'B';
    const char cmd[] = {'K', kEchoByte};
    if (!write_all(fd, cmd, sizeof(cmd))) {
        close(fd);
        return false;
    }

    std::string reply;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline && reply.size() < 8) {
        char ch = 0;
        const ssize_t r = read(fd, &ch, 1);
        if (r == 1) {
            reply.push_back(ch);
            if (ch == '#') {
                break;
            }
        } else if (r < 0 && errno != EAGAIN && errno != EINTR) {
            break;
        }
    }
    close(fd);
    return reply.size() >= 2 && reply[reply.size() - 2] == kEchoByte && reply.back() == '#';
}

}  // namespace alpacacore::util

#endif  // !_WIN32
