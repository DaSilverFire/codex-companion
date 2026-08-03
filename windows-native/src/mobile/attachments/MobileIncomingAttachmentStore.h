#pragma once

#include "codex/attachments/AttachmentStore.h"

namespace companion {

class MobileIncomingAttachmentStore final {
public:
    MobileIncomingAttachmentStore();
    explicit MobileIncomingAttachmentStore(
        QString rootPath,
        AttachmentClock clock = {},
        AttachmentWriter writer = {});

    static Result<void> validate(
        const QVector<BridgeAttachment>& attachments);

    Result<QVector<StagedAttachment>> stage(
        const QVector<BridgeAttachment>& attachments,
        const QUuid& requestId) const;
    Result<StagedAttachmentBatch> stageOwned(
        const QVector<BridgeAttachment>& attachments,
        const QUuid& requestId) const;

private:
    AttachmentStore store_;
};

} // namespace companion
