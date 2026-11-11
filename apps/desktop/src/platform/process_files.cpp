#include "process_files.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cerrno>
#include <csignal>
#include <limits>
#include <vector>
#ifdef Q_OS_MACOS
#include <libproc.h>
#include <sys/proc_info.h>
#endif

namespace lapis::desktop::platform {
namespace {
bool valid_pid(qint64 pid) { return pid > 0 && pid <= std::numeric_limits<pid_t>::max(); }

// The processes whose parent is `pid`.
QList<qint64> children(qint64 pid) {
    QList<qint64> found;
#ifdef Q_OS_MACOS
    std::vector<pid_t> pids(256);
    const int count = ::proc_listchildpids(static_cast<pid_t>(pid), pids.data(),
                                           static_cast<int>(pids.size() * sizeof(pid_t)));
    for (int index = 0; index < count && index < static_cast<int>(pids.size()); ++index)
        if (pids[std::size_t(index)] > 0)
            found.append(pids[std::size_t(index)]);
#elif defined(Q_OS_LINUX)
    // /proc/<pid>/stat: "pid (name) state ppid ..."; the name may hold spaces
    // or parentheses, so the fields after it start at its last ')'.
    const QDir proc(QStringLiteral("/proc"));
    for (const auto& name : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool number = false;
        const auto child = name.toLongLong(&number);
        if (!number)
            continue;
        QFile stat(proc.filePath(name + QStringLiteral("/stat")));
        if (!stat.open(QIODevice::ReadOnly))
            continue;
        const auto line = stat.readLine(4096);
        const auto fields = line.mid(line.lastIndexOf(')') + 2).split(' ');
        if (fields.size() > 1 && fields.at(1).toLongLong() == pid)
            found.append(child);
    }
#else
    static_cast<void>(pid);
#endif
    return found;
}
} // namespace

bool process_alive(qint64 pid) {
    return valid_pid(pid) && (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM);
}

QList<qint64> process_tree(qint64 pid, int depth, int limit) {
    if (!process_alive(pid))
        return {};
    QList<qint64> tree{pid};
    for (qsizetype start = 0; depth > 0 && start < tree.size(); --depth) {
        const auto end = tree.size();
        for (auto index = start; index < end; ++index)
            for (const auto child : children(tree.at(index)))
                if (tree.size() < limit && !tree.contains(child))
                    tree.append(child);
        start = end;
    }
    return tree;
}

QStringList open_files(qint64 pid) {
    QStringList paths;
    if (!valid_pid(pid))
        return paths;
#ifdef Q_OS_MACOS
    const auto process = static_cast<pid_t>(pid);
    const int needed = ::proc_pidinfo(process, PROC_PIDLISTFDS, 0, nullptr, 0);
    if (needed <= 0)
        return paths;
    // Room for descriptors opened between the two calls.
    std::vector<proc_fdinfo> fds(std::size_t(needed) / sizeof(proc_fdinfo) + 32);
    const int bytes = ::proc_pidinfo(process, PROC_PIDLISTFDS, 0, fds.data(),
                                     static_cast<int>(fds.size() * sizeof(proc_fdinfo)));
    const auto count = bytes > 0 ? std::size_t(bytes) / sizeof(proc_fdinfo) : 0;
    for (std::size_t index = 0; index < count && index < fds.size(); ++index) {
        if (fds[index].proc_fdtype != PROX_FDTYPE_VNODE)
            continue;
        vnode_fdinfowithpath info{};
        if (::proc_pidfdinfo(process, fds[index].proc_fd, PROC_PIDFDVNODEPATHINFO, &info,
                             PROC_PIDFDVNODEPATHINFO_SIZE) == PROC_PIDFDVNODEPATHINFO_SIZE &&
            info.pvip.vip_path[0] != '\0')
            paths.append(QFile::decodeName(info.pvip.vip_path));
    }
#elif defined(Q_OS_LINUX)
    const QDir fds(QStringLiteral("/proc/%1/fd").arg(pid));
    for (const auto& name : fds.entryList(QDir::System | QDir::NoDotAndDotDot))
        if (const auto target = QFileInfo(fds.filePath(name)).symLinkTarget();
            target.startsWith(QLatin1Char('/')))
            paths.append(target);
#endif
    return paths;
}
} // namespace lapis::desktop::platform
