#pragma once

#include <QHash>
#include <QString>

namespace companion {

enum class ThreadRuntimeStatus {
    NotLoaded,
    Idle,
    Active,
    WaitingOnApproval,
    WaitingOnUserInput,
    SystemError,
};

struct ThreadRuntimeSnapshot final {
    QHash<QString, ThreadRuntimeStatus> statuses;
    bool authoritative = false;

    friend bool operator==(
        const ThreadRuntimeSnapshot&,
        const ThreadRuntimeSnapshot&) = default;
};

} // namespace companion
