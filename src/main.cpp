// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/signalfd.h>
#include <unistd.h>

#include "api.h"
#include "failure.h"
#include "io.h"
#include "json.h"
#include "listing.h"
#include "log.h"
#include "presence.h"
#include "session.h"
#include "settings.h"
#include "watchdog.h"

using namespace fern;

namespace {

const char usage_text[] =
    "usage: fern-sdrplay --describe          print the module and its settings as JSON\n"
    "       fern-sdrplay --list-devices      print the RSPs the SDRplay API offers as JSON\n"
    "       fern-sdrplay --fernsdr-module 1  run as a FernSDR input module (FernSDR starts it so)\n"
    "       fern-sdrplay --notices           print the licence and what the module needs from SDRplay\n"
    "       fern-sdrplay --version\n";

const char notices_text[] =
    "Fern-SDRPlay is free software under the GNU General Public License, version 2 or (at your option) any\n"
    "later version, with an additional permission: it may load and use SDRplay's proprietary API library\n"
    "(libsdrplay_api) at run time, and a work that combines the two may be conveyed under the GPL for this\n"
    "module's part and SDRplay's licence for theirs. The LICENSE file in the source says so in full.\n"
    "\n"
    "The module contains no part of SDRplay's software. It needs SDRplay API 3.14 or 3.15, which the operator\n"
    "installs from https://www.sdrplay.com/api/ and whose licence the operator accepts there. That licence\n"
    "(section 6) allows SDRplay's service to send details of the radio devices it drives to an SDRplay server.\n";

// Test builds take the library and the USB directory from the environment,
// so that the tests can run the real program against a fake API. Release
// builds use SDRplay's library and the kernel's list only.
Api make_api() {
#ifdef FERN_SDRPLAY_TESTING
    if (const char* path = std::getenv("FERN_SDRPLAY_LIBRARY"))
        return Api({path});
#endif
    return Api();
}

std::string usb_devices_dir() {
#ifdef FERN_SDRPLAY_TESTING
    if (const char* dir = std::getenv("FERN_SDRPLAY_USB_DIR"))
        return dir;
#endif
    return default_usb_devices_dir;
}

int print(const std::string& text) {
    return write_all(STDOUT_FILENO, text.data(), text.size()) == 0 ? 0 : exit_status::internal;
}

int describe() {
    const std::string text = json::serialize(describe_module()) + "\n";
    if (text.size() > max_report_bytes) {
        std::fprintf(stderr, "fern-sdrplay: the description is larger than %zu bytes\n", max_report_bytes);
        return exit_status::internal;
    }
    return print(text);
}

int list() {
    // FernSDR allows 10 seconds; a hung API call is reported within 8.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    Watchdog watchdog([](const std::string& what) {
        json::Value report = json::Value::object();
        report.set("devices", json::Value::array());
        report.set("error", what + " did not return in time. Another program may hold the SDRplay API's lock, or "
                                   "the sdrplay service hangs: sudo systemctl restart sdrplay");
        const std::string text = json::serialize(report) + "\n";
        (void)write_all(STDOUT_FILENO, text.data(), text.size(), 500);
        _exit(0);
    });
    Api api = make_api();
    // The library writes its own complaints to stderr (perror); stdout
    // carries the report only.
    return print(report_text(list_devices(api, watchdog, deadline, usb_devices_dir())));
}

int module() {
    for (int fd : {STDIN_FILENO, STDOUT_FILENO, 3}) {
        if (::fcntl(fd, F_GETFD) < 0) {
            std::fprintf(stderr,
                         "fern-sdrplay: fd %d is not open. --fernsdr-module is how FernSDR runs this program, "
                         "with commands on fd 0, samples on fd 1 and events on fd 3.\n",
                         fd);
            return exit_status::usage;
        }
    }
    std::signal(SIGPIPE, SIG_IGN);

    // SIGTERM and friends arrive through a signalfd, so that the session
    // loop sees them like any other input. Threads started later, the API's
    // among them, inherit the mask.
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGTERM);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGHUP);
    if (pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr) != 0) {
        log_line("could not block the stop signals");
        return exit_status::internal;
    }
    const int signal_fd = signalfd(-1, &stop_signals, SFD_CLOEXEC);
    if (signal_fd < 0) {
        log_line("could not create a signalfd: %s", std::strerror(errno));
        return exit_status::internal;
    }

    log_line("fern-sdrplay %s", module_version());
    Api api = make_api();
    SessionIo io;
    io.stop = signal_fd;
    SessionOptions options;
    options.usb_devices_dir = usb_devices_dir();
#ifdef FERN_SDRPLAY_TESTING
    // So that the command line tests see a hung API call end the process
    // without waiting 12 seconds.
    if (const char* ms = std::getenv("FERN_SDRPLAY_OPEN_TIMEOUT_MS"))
        options.open_timeout = std::chrono::milliseconds(std::atoi(ms));
#endif
    const SessionResult result = run_session(api, io, options);
    if (!result.clean)
        _exit(result.status);
    ::close(signal_fd);
    // The API's threads may still be winding down after sdrplay_api_Close;
    // _exit skips the static destructors they could race with.
    _exit(result.status);
}

}  // namespace

int main(int argc, char** argv) {
    const int n = argc - 1;
    const std::string_view first = n >= 1 ? argv[1] : "";
    if (n == 1 && first == "--describe")
        return describe();
    if (n == 1 && first == "--list-devices")
        return list();
    if (n == 1 && first == "--notices")
        return print(notices_text);
    if (n == 2 && first == "--fernsdr-module") {
        if (std::string_view(argv[2]) == "1")
            return module();
        std::fprintf(stderr, "fern-sdrplay: this module speaks module API 1, not %s\n", argv[2]);
        return exit_status::usage;
    }
    if (n == 1 && first == "--version")
        return print(std::string("fern-sdrplay ") + module_version() + "\n");
    if (n == 1 && (first == "--help" || first == "-h"))
        return print(usage_text);
    std::fputs(usage_text, stderr);
    return exit_status::usage;
}
