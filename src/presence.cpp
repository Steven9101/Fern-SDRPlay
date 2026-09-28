// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include "presence.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace fern {

int count_rsps_on_usb(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (!d)
        return -1;
    int count = 0;
    while (const dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.')
            continue;
        // Interfaces ("1-1:1.0") have no idVendor; devices and hubs do.
        const std::string path = dir + "/" + e->d_name + "/idVendor";
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        char buf[8] = {};
        const ssize_t n = ::read(fd, buf, sizeof buf - 1);
        ::close(fd);
        if (n >= 4 && buf[0] == '1' && buf[1] == 'd' && buf[2] == 'f' && buf[3] == '7')
            ++count;
    }
    ::closedir(d);
    return count;
}

}  // namespace fern
