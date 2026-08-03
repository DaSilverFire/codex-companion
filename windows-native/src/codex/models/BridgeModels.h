#pragma once

#include <QByteArray>
#include <QString>
#include <QUuid>
#include <QVector>
#include <QtGlobal>

#include <optional>

namespace companion {

inline constexpr qint64 kBridgeProtocolVersion = 1;
inline constexpr qint64 kDefaultTaskPageSize = 20;
inline constexpr qint64 kDefaultMessagePageSize = 30;
inline constexpr qint64 kMaximumPageSize = 50;

struct BridgeDate final {
    double secondsSinceReferenceDate = 0.0;
    friend bool operator==(const BridgeDate&, const BridgeDate&) = default;
};

enum class BridgeWireProfile { NearbyV1Milliseconds, RelayV1Canonical };
enum class BridgeFeature {
    TaskStreamV1,
    PresencePetPackageV1,
    AttachmentUploadV1,
};
enum class BridgeOperation {
    Handshake, ListTasks, LoadMessages, SendMessage, RespondToApproval,
    CreateTask, LoadCapabilities, SendCasualChat, LoadUsage,
    ConsumeUsageReset, CreateGoal, ResumeGoal, UpdateGoal,
    LoadPresencePetManifest, LoadPresencePetChunk,
};
enum class SendAction { Reply, Steer };
enum class ChatProvider { OnDevice, OpenAIAPI, LumoAPI };
enum class ApprovalDecision { ApproveOnce, ApproveSimilar, Decline };
enum class TaskStatus { Running, Waiting, Completed, Failed };
enum class GoalStatus {
    Active, Paused, Blocked, UsageLimited, BudgetLimited, Complete,
};
enum class TaskGroupKind { Chats, Project };
enum class AttachmentKind { File, Image };
enum class MessageRole { User, Assistant };
enum class TimelineKind { Message, Reasoning, Tool, Status, Compaction };
enum class TimelineStatus { InProgress, Completed, Failed };
enum class TimelinePhase { Commentary, Final };
enum class MediaKind { Image };
enum class BridgePresencePetState { Idle, Thinking, Talking };

struct BridgeAttachment final {
    QUuid id;
    AttachmentKind kind = AttachmentKind::File;
    QString filename;
    std::optional<QString> mimeType;
    QByteArray data;
    friend bool operator==(const BridgeAttachment&, const BridgeAttachment&) = default;
};

struct BridgeGoal final {
    QString threadId;
    QString objective;
    GoalStatus status = GoalStatus::Active;
    std::optional<qint64> tokenBudget;
    qint64 tokensUsed = 0;
    qint64 elapsedSeconds = 0;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;
    friend bool operator==(const BridgeGoal&, const BridgeGoal&) = default;
};

struct BridgeTaskGroup final {
    TaskGroupKind kind = TaskGroupKind::Chats;
    QString title;
    std::optional<QString> path;
    friend bool operator==(const BridgeTaskGroup&, const BridgeTaskGroup&) = default;
};

struct BridgeTask final {
    QString id;
    QString title;
    QString preview;
    BridgeDate updatedAt;
    std::optional<QString> cwd;
    TaskStatus status = TaskStatus::Waiting;
    bool needsApproval = false;
    std::optional<QString> activeTurnId;
    std::optional<QString> model;
    std::optional<QString> reasoningEffort;
    std::optional<BridgeTaskGroup> taskGroup;
    std::optional<BridgeGoal> goal;
    QString rolloutPath;
    friend bool operator==(const BridgeTask&, const BridgeTask&) = default;
};

struct BridgeMessage final {
    QString id;
    MessageRole role = MessageRole::User;
    QString text;
    std::optional<BridgeDate> createdAt;
    std::optional<QVector<BridgeAttachment>> attachments;
    friend bool operator==(const BridgeMessage&, const BridgeMessage&) = default;
};

struct BridgeMedia final {
    QString id;
    MediaKind kind = MediaKind::Image;
    QString mimeType;
    QByteArray data;
    friend bool operator==(const BridgeMedia&, const BridgeMedia&) = default;
};

struct BridgeTimelineItem final {
    QString id;
    TimelineKind kind = TimelineKind::Status;
    TimelineStatus status = TimelineStatus::Completed;
    std::optional<MessageRole> role;
    std::optional<QString> title;
    std::optional<QString> text;
    std::optional<QString> detail;
    std::optional<TimelinePhase> phase;
    std::optional<BridgeDate> createdAt;
    std::optional<QString> turnId;
    std::optional<QString> callId;
    QVector<BridgeMedia> media;
    friend bool operator==(const BridgeTimelineItem&, const BridgeTimelineItem&) = default;
};

struct BridgeSubagent final {
    QString id;
    QString name;
    QString title;
    std::optional<QString> role;
    BridgeDate updatedAt;
    TaskStatus status = TaskStatus::Waiting;
    std::optional<bool> needsApproval;
    friend bool operator==(const BridgeSubagent&, const BridgeSubagent&) = default;
};

struct BridgeContextUsage final {
    qint64 usedTokens = 0;
    qint64 contextWindow = 0;
    friend bool operator==(const BridgeContextUsage&, const BridgeContextUsage&) = default;
};

struct BridgeUsageWindow final {
    double remainingPercent = 0.0;
    QString durationLabel;
    std::optional<BridgeDate> resetsAt;
    friend bool operator==(const BridgeUsageWindow&, const BridgeUsageWindow&) = default;
};

struct BridgeResetCredit final {
    QString id;
    QString displayTitle;
    std::optional<QString> detail;
    std::optional<BridgeDate> expiresAt;
    friend bool operator==(const BridgeResetCredit&, const BridgeResetCredit&) = default;
};

struct BridgeUsageGroup final {
    QString id;
    QString title;
    std::optional<BridgeUsageWindow> shortWindow;
    std::optional<BridgeUsageWindow> weeklyWindow;
    friend bool operator==(const BridgeUsageGroup&, const BridgeUsageGroup&) = default;
};

struct BridgeUsageSnapshot final {
    std::optional<QString> planType;
    QVector<BridgeUsageGroup> groups;
    qint64 availableResetCount = 0;
    QVector<BridgeResetCredit> availableResetCredits;
    BridgeDate updatedAt;
    std::optional<QString>
        rateLimitReachedType;
    friend bool operator==(const BridgeUsageSnapshot&, const BridgeUsageSnapshot&) = default;
};

struct BridgeReasoningEffort final {
    QString value;
    QString description;
    friend bool operator==(const BridgeReasoningEffort&, const BridgeReasoningEffort&) = default;
};

struct BridgeModel final {
    QString id;
    QString model;
    QString displayName;
    QString description;
    bool isDefault = false;
    QString defaultReasoningEffort;
    QVector<BridgeReasoningEffort> supportedReasoningEfforts;
    friend bool operator==(const BridgeModel&, const BridgeModel&) = default;
};

struct BridgeSkill final {
    QString name;
    QString displayName;
    QString description;
    QString path;
    QString scope;
    std::optional<QString> defaultPrompt;
    friend bool operator==(const BridgeSkill&, const BridgeSkill&) = default;
};

struct BridgePlugin final {
    QString id;
    QString name;
    QString displayName;
    QString description;
    bool enabled = false;
    bool installed = false;
    friend bool operator==(const BridgePlugin&, const BridgePlugin&) = default;
};

struct BridgeChatAgent final {
    QString id;
    QString name;
    QString description;
    QString symbolName;
    friend bool operator==(const BridgeChatAgent&, const BridgeChatAgent&) = default;
};

struct BridgeChatModel final {
    QString id;
    ChatProvider provider = ChatProvider::OnDevice;
    QString model;
    QString displayName;
    QString description;
    bool isDefault = false;
    bool isAvailable = false;
    bool supportsAttachments = false;
    friend bool operator==(const BridgeChatModel&, const BridgeChatModel&) = default;
};

struct BridgeCapabilities final {
    QVector<BridgeModel> models;
    QVector<BridgeSkill> skills;
    QVector<BridgePlugin> plugins;
    QVector<BridgeChatAgent> chatAgents;
    std::optional<QVector<BridgeChatModel>> chatModels;
    friend bool operator==(const BridgeCapabilities&, const BridgeCapabilities&) = default;
};

struct BridgePresencePetFile final {
    QString name;
    QString sha256;
    qint64 byteCount = 0;
    friend bool operator==(
        const BridgePresencePetFile&,
        const BridgePresencePetFile&) = default;
};

struct BridgePresencePetAtlas final {
    BridgePresencePetFile file;
    qint64 cellWidth = 0;
    qint64 cellHeight = 0;
    qint64 columns = 0;
    qint64 rows = 0;
    friend bool operator==(
        const BridgePresencePetAtlas&,
        const BridgePresencePetAtlas&) = default;
};

struct BridgePresencePetAnimation final {
    BridgePresencePetState state =
        BridgePresencePetState::Idle;
    qint64 row = 0;
    qint64 frameCount = 0;
    QVector<qint64> frameDurationsMilliseconds;
    qint64 posterFrame = 0;
    friend bool operator==(
        const BridgePresencePetAnimation&,
        const BridgePresencePetAnimation&) = default;
};

struct BridgePresencePetManifest final {
    qint64 schemaVersion = 0;
    QString packageId;
    QString petId;
    QString displayName;
    QString assetVersion;
    BridgePresencePetAtlas atlas;
    BridgePresencePetFile thumbnail;
    QVector<BridgePresencePetAnimation> animations;
    QString contentHash;
    friend bool operator==(
        const BridgePresencePetManifest&,
        const BridgePresencePetManifest&) = default;
};

struct BridgePresencePetCatalogEntry final {
    QString packageId;
    QString petId;
    QString displayName;
    QString assetVersion;
    QString contentHash;
    qint64 byteCount = 0;
    BridgePresencePetFile thumbnail;
    friend bool operator==(
        const BridgePresencePetCatalogEntry&,
        const BridgePresencePetCatalogEntry&) = default;
};

struct BridgePresencePetChunk final {
    QString packageId;
    QString contentHash;
    QString fileName;
    qint64 offset = 0;
    QByteArray data;
    qint64 nextOffset = 0;
    bool isComplete = false;
    friend bool operator==(
        const BridgePresencePetChunk&,
        const BridgePresencePetChunk&) = default;
};

struct BridgeRequest final {
    QUuid id;
    qint64 protocolVersion = kBridgeProtocolVersion;
    BridgeOperation operation = BridgeOperation::Handshake;
    std::optional<QString> cursor;
    std::optional<qint64> limit;
    std::optional<QString> threadId;
    std::optional<QString> goalObjective;
    std::optional<qint64> goalTokenBudget;
    std::optional<QString> text;
    std::optional<QString> cwd;
    std::optional<SendAction> sendAction;
    std::optional<ApprovalDecision> approvalDecision;
    std::optional<QString> model;
    std::optional<QString> reasoningEffort;
    std::optional<QString> skillName;
    std::optional<QString> skillPath;
    std::optional<QString> chatAgentId;
    std::optional<ChatProvider> chatProvider;
    std::optional<QString> chatModelId;
    std::optional<QString> resetCreditId;
    std::optional<QVector<BridgeAttachment>> attachments;
    std::optional<QUuid> idempotencyKey;
    std::optional<QString> presencePetPackageId;
    std::optional<QString> presencePetContentHash;
    std::optional<QString> presencePetFileName;
    std::optional<qint64> presencePetOffset;
    std::optional<qint64> presencePetLength;
    friend bool operator==(const BridgeRequest&, const BridgeRequest&) = default;
};

struct BridgeResponse final {
    QUuid id;
    qint64 protocolVersion = kBridgeProtocolVersion;
    BridgeOperation operation = BridgeOperation::Handshake;
    bool succeeded = false;
    std::optional<QString> errorCode;
    std::optional<QString> message;
    std::optional<QString> macName;
    std::optional<QString> macDeviceId;
    std::optional<QByteArray> pairingSecret;
    std::optional<QString> relayUrlString;
    std::optional<QVector<BridgeTask>> tasks;
    std::optional<QVector<BridgeMessage>> messages;
    std::optional<QString> nextCursor;
    std::optional<QString> threadId;
    std::optional<BridgeCapabilities> capabilities;
    std::optional<BridgeMessage> chatMessage;
    std::optional<QVector<BridgeTimelineItem>> timelineItems;
    std::optional<QString> revision;
    std::optional<QString> timelineNextCursor;
    std::optional<QVector<BridgeSubagent>> subagents;
    std::optional<BridgeContextUsage> contextUsage;
    std::optional<BridgeUsageSnapshot> usageSnapshot;
    std::optional<BridgeGoal> goal;
    std::optional<QVector<BridgeFeature>> features;
    std::optional<QString> selectedDesktopPetId;
    std::optional<QVector<BridgePresencePetCatalogEntry>>
        presencePetCatalog;
    std::optional<BridgePresencePetManifest>
        presencePetManifest;
    std::optional<BridgePresencePetChunk>
        presencePetChunk;
    friend bool operator==(const BridgeResponse&, const BridgeResponse&) = default;
};

} // namespace companion
