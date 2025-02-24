// Copyright 2020 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "syschecks/syschecks.h"

#include "base/likely.h"
#include "base/seastarx.h"
#include "version.h"

#include <seastar/core/coroutine.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/posix.hh> // here for workaround
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/api.hh>

namespace {
ss::sstring to_string(ss::fs_type fs) {
    switch (fs) {
    case ss::fs_type::other:
        return "other";
    case ss::fs_type::xfs:
        return "xfs";
    case ss::fs_type::ext2:
        return "ext2";
    case ss::fs_type::ext3:
        return "ext3";
    case ss::fs_type::ext4:
        return "ext4";
    case ss::fs_type::btrfs:
        return "btrfs";
    case ss::fs_type::hfs:
        return "hfs";
    case ss::fs_type::tmpfs:
        return "tmpfs";
    };
    return "bad_enum";
}
} // namespace

namespace syschecks {
ss::logger checklog{"syschecks"};

ss::future<> disk(const ss::sstring& path) {
    return ss::check_direct_io_support(path).then([path] {
        return ss::file_system_at(path).then([path](auto fs) {
            checklog.info0("Detected file system type is {}", to_string(fs));
            // Currently, all of ext2, 3 and 4 are detected as ext2, so we just
            // assume an ext2 detection means ext4 for now, see:
            // https://github.com/redpanda-data/redpanda/issues/13469
            // We also still check for ext4, since if that is returned it means
            // that seastar has been fixed to be able to detect ext4.
            if (fs == ss::fs_type::ext2 || fs == ss::fs_type::ext4) {
                checklog.warn(
                  "Path: `{}' is on ext4, not XFS. This will probably work, "
                  "but Redpanda is only tested on XFS and XFS is recommended "
                  "for best performance.",
                  path);
            } else if (fs != ss::fs_type::xfs) {
                checklog.error(
                  "Path: `{}' uses {} filesystem which is not XFS or ext4. "
                  "This is a unsupported "
                  "configuration. You may experience poor performance or "
                  "instability.",
                  path,
                  to_string(fs));
            }
        });
    });
}

void memory(bool ignore) {
    static const uint64_t kMinMemory = 1 << 30;
    const auto shard_mem = ss::memory::stats().total_memory();
    if (shard_mem >= kMinMemory) {
        return;
    }
    auto line = fmt::format(
      "Memory: '{}' below recommended: '{}'", shard_mem, kMinMemory);
    checklog.error(line.c_str());
    if (!ignore) {
        throw std::runtime_error(line);
    }
}

ss::future<> systemd_notify_ready() {
    ss::sstring msg = ssx::sformat(
      "READY=1\nSTATUS=redpanda is ready! - {}", redpanda_version());
    return systemd_raw_message(std::move(msg));
}

ss::future<> systemd_raw_message(ss::sstring out) {
    const char* systemd_socket_path = std::getenv("NOTIFY_SOCKET");
    std::string_view log_msg = out;
    if (out.back() == '\n') {
        // emit tight logs without extra new lines
        log_msg = std::string_view(out.c_str(), out.size() - 1);
    }
    if (!systemd_socket_path) {
        checklog.trace("NOTIFY_SOCKET unset. ignoring {}", log_msg);
        co_return;
    }
    checklog.info("{}", log_msg);
    ss::sstring systemd_socket = systemd_socket_path;
    if (systemd_socket[0] == '@') {
        // detected abstract socket; replace @ with 0
        systemd_socket[0] = '\0';
    }
    auto nixaddr = ss::unix_domain_addr(systemd_socket);
    auto addr = ss::socket_address(nixaddr);
    /**
    // NOTE: the below impl works once we fix seastar
    auto chan = ss::make_udp_channel();
    try {
        co_await chan.send(addr, out.data());
    } catch (const std::exception& e) {
        checklog.error("Error sending systemd notification: {}", e.what());
    }
    chan.shutdown_input();
    chan.shutdown_output();
    */

    // TODO: remove once we futurize the channel code above
    // In the meantime, we are ok to block while we make this one syscall
    // it is used only in main() before anything actually starts.
    auto fd = ss::file_desc::socket(AF_UNIX, SOCK_DGRAM, 0);
    fd.sendto(addr, out.data(), out.length(), 0);
    co_return;
}

} // namespace syschecks
