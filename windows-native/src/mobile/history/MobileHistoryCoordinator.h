#pragma once

#include "codex/state/HistoryCoordinator.h"

#include <QFuture>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

class QObject;

namespace companion {

struct MobileHistoryKey final {
    QString threadId;
    std::optional<QString> cursor;
    int limit =
        static_cast<int>(kDefaultMessagePageSize);

    friend bool operator==(
        const MobileHistoryKey&,
        const MobileHistoryKey&) = default;
};

using MobileHistoryCompletion =
    std::function<void(Result<HistorySnapshot>)>;

class MobileHistoryCoordinator final {
public:
    MobileHistoryCoordinator();
    explicit MobileHistoryCoordinator(
        std::shared_ptr<HistoryCoordinator> coordinator);

    QFuture<Result<HistorySnapshot>> load(
        MobileHistoryKey key,
        HistoryLoader operation);

    QFuture<Result<HistorySnapshot>> loadSelected(
        MobileHistoryKey key,
        HistoryLoader operation,
        QObject* receiver,
        MobileHistoryCompletion completion);

    static Result<QString> revisionForFile(
        const QString& path);

private:
    struct State;

    static HistoryKey normalizedKey(
        MobileHistoryKey key);

    std::shared_ptr<State> state_;
};

} // namespace companion
