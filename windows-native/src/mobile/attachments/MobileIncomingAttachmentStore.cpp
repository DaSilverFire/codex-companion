#include "mobile/attachments/MobileIncomingAttachmentStore.h"

#include <utility>

namespace companion {

MobileIncomingAttachmentStore::
MobileIncomingAttachmentStore() = default;

MobileIncomingAttachmentStore::
MobileIncomingAttachmentStore(
    QString rootPath,
    AttachmentClock clock,
    AttachmentWriter writer)
    : store_(
          std::move(rootPath),
          std::move(clock),
          std::move(writer))
{
}

Result<void> MobileIncomingAttachmentStore::validate(
    const QVector<BridgeAttachment>& attachments)
{
    return AttachmentStore::validate(attachments);
}

Result<QVector<StagedAttachment>>
MobileIncomingAttachmentStore::stage(
    const QVector<BridgeAttachment>& attachments,
    const QUuid& requestId) const
{
    return store_.stage(attachments, requestId);
}

Result<StagedAttachmentBatch>
MobileIncomingAttachmentStore::stageOwned(
    const QVector<BridgeAttachment>& attachments,
    const QUuid& requestId) const
{
    return store_.stageOwned(attachments, requestId);
}

} // namespace companion
