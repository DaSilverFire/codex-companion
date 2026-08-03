#include "updater-helper/UpdateInstallRequest.h"

#include "update/ReleaseVersion.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <QVariantMap>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ShlObj.h>

namespace companion {
namespace {

constexpr qint64 kMaximumRequestBytes =
    64 * 1024;
constexpr qint64 kMaximumInstallerBytes =
    512LL * 1024LL * 1024LL;

CompanionError requestError(
    QString code,
    QString message,
    QVariantMap context = {})
{
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

Result<UpdateInstallRequest>
invalidRequest(QString message)
{
    return Result<UpdateInstallRequest>::
        failure(
            requestError(
                QStringLiteral(
                    "update.install_request_invalid"),
                std::move(message)));
}

Result<void> invalidRequestValue(
    QStringView field,
    QString message)
{
    return Result<void>::failure(
        requestError(
            QStringLiteral(
                "update.install_request_invalid"),
            std::move(message),
            {
                {
                    QStringLiteral("field"),
                    field.toString(),
                },
            }));
}

class JsonDuplicateKeyScanner final {
public:
    explicit JsonDuplicateKeyScanner(
        QByteArrayView bytes)
        : bytes_(
              bytes.data(),
              bytes.size())
    {
    }

    bool scan()
    {
        skipWhitespace();
        if (!parseValue()) {
            return false;
        }
        skipWhitespace();
        return position_ == bytes_.size();
    }

    bool hasDuplicate() const noexcept
    {
        return duplicate_;
    }

private:
    bool parseValue()
    {
        skipWhitespace();
        if (position_ >= bytes_.size()) {
            return false;
        }

        const char current =
            bytes_.at(position_);
        if (current == '{') {
            return parseObject();
        }
        if (current == '[') {
            return parseArray();
        }
        if (current == '"') {
            return readString(nullptr);
        }
        if (current == '-'
            || (current >= '0'
                && current <= '9')) {
            return skipNumber();
        }
        return skipLiteral("true")
            || skipLiteral("false")
            || skipLiteral("null");
    }

    bool parseObject()
    {
        ++position_;
        skipWhitespace();
        if (consume('}')) {
            return true;
        }

        QSet<QString> keys;
        while (position_ < bytes_.size()) {
            QString key;
            if (!readString(&key)) {
                return false;
            }
            if (keys.contains(key)) {
                duplicate_ = true;
            }
            keys.insert(std::move(key));

            skipWhitespace();
            if (!consume(':')
                || !parseValue()) {
                return false;
            }
            skipWhitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    bool parseArray()
    {
        ++position_;
        skipWhitespace();
        if (consume(']')) {
            return true;
        }

        while (position_ < bytes_.size()) {
            if (!parseValue()) {
                return false;
            }
            skipWhitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    static bool isHexDigit(char value)
    {
        return (value >= '0'
                && value <= '9')
            || (value >= 'a'
                && value <= 'f')
            || (value >= 'A'
                && value <= 'F');
    }

    bool readString(QString* value)
    {
        if (position_ >= bytes_.size()
            || bytes_.at(position_) != '"') {
            return false;
        }

        const qsizetype start = position_;
        ++position_;
        while (position_ < bytes_.size()) {
            const char current =
                bytes_.at(position_++);
            if (current == '\\') {
                if (position_ >= bytes_.size()) {
                    return false;
                }
                const char escape =
                    bytes_.at(position_++);
                if (escape == 'u') {
                    if (position_ + 4
                        > bytes_.size()) {
                        return false;
                    }
                    for (qsizetype index = 0;
                         index < 4;
                         ++index) {
                        if (!isHexDigit(
                                bytes_.at(
                                    position_
                                    + index))) {
                            return false;
                        }
                    }
                    position_ += 4;
                } else if (
                    escape != '"'
                    && escape != '\\'
                    && escape != '/'
                    && escape != 'b'
                    && escape != 'f'
                    && escape != 'n'
                    && escape != 'r'
                    && escape != 't') {
                    return false;
                }
            } else if (current == '"') {
                if (value != nullptr) {
                    QByteArray wrapped("[");
                    wrapped.append(
                        bytes_.mid(
                            start,
                            position_ - start));
                    wrapped.append(']');
                    QJsonParseError error;
                    const QJsonDocument
                        document =
                            QJsonDocument::
                                fromJson(
                                    wrapped,
                                    &error);
                    if (error.error
                            != QJsonParseError::
                                NoError
                        || !document.isArray()
                        || document.array()
                                   .size()
                            != 1
                        || !document.array()
                                .at(0)
                                .isString()) {
                        return false;
                    }
                    *value =
                        document.array()
                            .at(0)
                            .toString();
                }
                return true;
            } else if (
                static_cast<unsigned char>(
                    current)
                < 0x20U) {
                return false;
            }
        }
        return false;
    }

    bool skipNumber()
    {
        if (consume('-')
            && position_ >= bytes_.size()) {
            return false;
        }
        if (!consume('0')) {
            if (position_ >= bytes_.size()
                || bytes_.at(position_) < '1'
                || bytes_.at(position_) > '9') {
                return false;
            }
            ++position_;
            while (
                position_ < bytes_.size()
                && bytes_.at(position_) >= '0'
                && bytes_.at(position_) <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            const qsizetype start =
                position_;
            while (
                position_ < bytes_.size()
                && bytes_.at(position_) >= '0'
                && bytes_.at(position_) <= '9') {
                ++position_;
            }
            if (position_ == start) {
                return false;
            }
        }
        if (position_ < bytes_.size()
            && (bytes_.at(position_) == 'e'
                || bytes_.at(position_)
                    == 'E')) {
            ++position_;
            if (!consume('+')) {
                consume('-');
            }
            const qsizetype start =
                position_;
            while (
                position_ < bytes_.size()
                && bytes_.at(position_) >= '0'
                && bytes_.at(position_) <= '9') {
                ++position_;
            }
            if (position_ == start) {
                return false;
            }
        }
        return true;
    }

    bool skipLiteral(
        QByteArrayView literal)
    {
        if (bytes_.mid(
                position_,
                literal.size())
            != literal) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    bool consume(char expected)
    {
        if (position_ >= bytes_.size()
            || bytes_.at(position_)
                != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    void skipWhitespace()
    {
        while (position_ < bytes_.size()) {
            const char current =
                bytes_.at(position_);
            if (current != ' '
                && current != '\t'
                && current != '\r'
                && current != '\n') {
                break;
            }
            ++position_;
        }
    }

    QByteArray bytes_;
    qsizetype position_ = 0;
    bool duplicate_ = false;
};

bool hasExactFields(
    const QJsonObject& object)
{
    static const std::array expected{
        QStringLiteral("requestId"),
        QStringLiteral("installerPath"),
        QStringLiteral("expectedSha256"),
        QStringLiteral("expectedSize"),
        QStringLiteral("expectedVersion"),
        QStringLiteral("expectedBuild"),
        QStringLiteral("installRoot"),
        QStringLiteral("rollbackRoot"),
        QStringLiteral(
            "uninstallRegistryKey"),
        QStringLiteral(
            "startMenuShortcut"),
        QStringLiteral(
            "acknowledgementEvent"),
        QStringLiteral(
            "parentProcessId"),
    };
    if (object.size()
        != static_cast<qsizetype>(
            expected.size())) {
        return false;
    }
    return std::all_of(
        expected.cbegin(),
        expected.cend(),
        [&object](const QString& key) {
            return object.contains(key);
        });
}

std::optional<qint64> integerValue(
    const QJsonValue& value)
{
    if (!value.isDouble()) {
        return std::nullopt;
    }
    constexpr qint64 lowFallback =
        std::numeric_limits<qint64>::min();
    constexpr qint64 highFallback =
        std::numeric_limits<qint64>::max();
    const qint64 withLowFallback =
        value.toInteger(lowFallback);
    const qint64 withHighFallback =
        value.toInteger(highFallback);
    if (withLowFallback
        != withHighFallback) {
        return std::nullopt;
    }
    return withLowFallback;
}

bool isAsciiHexDigest(QStringView value)
{
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(
        value.begin(),
        value.end(),
        [](QChar character) {
            return (character
                        >= QLatin1Char('0')
                    && character
                        <= QLatin1Char('9'))
                || (character
                        >= QLatin1Char('a')
                    && character
                        <= QLatin1Char('f'))
                || (character
                        >= QLatin1Char('A')
                    && character
                        <= QLatin1Char('F'));
        });
}

QString normalizedPath(
    QStringView path)
{
    return QDir::cleanPath(
        QDir::fromNativeSeparators(
            path.toString()));
}

bool isCleanAbsoluteLocalPath(
    QStringView path)
{
    if (path.isEmpty()) {
        return false;
    }
    const QString normalized =
        normalizedPath(path);
    if (!QDir::isAbsolutePath(normalized)
        || normalized.size() < 3
        || normalized.at(1)
            != QLatin1Char(':')
        || normalized.at(2)
            != QLatin1Char('/')) {
        return false;
    }
    const QChar drive =
        normalized.at(0);
    if (!((drive >= QLatin1Char('A')
           && drive <= QLatin1Char('Z'))
          || (drive >= QLatin1Char('a')
              && drive
                  <= QLatin1Char('z')))) {
        return false;
    }
    const QString input =
        QDir::fromNativeSeparators(
            path.toString());
    if (input.endsWith(QLatin1Char('/'))
        || input.contains(
            QStringLiteral("/./"))
        || input.contains(
            QStringLiteral("/../"))
        || input.endsWith(
            QStringLiteral("/."))
        || input.endsWith(
            QStringLiteral("/.."))) {
        return false;
    }
    return normalized.compare(
               input,
               Qt::CaseInsensitive)
        == 0;
}

bool sameWindowsPath(
    QStringView left,
    QStringView right)
{
    return normalizedPath(left).compare(
               normalizedPath(right),
               Qt::CaseInsensitive)
        == 0;
}

bool isWithinTree(
    QStringView root,
    QStringView candidate)
{
    const QString normalizedRoot =
        normalizedPath(root);
    const QString normalizedCandidate =
        normalizedPath(candidate);
    return normalizedCandidate.compare(
               normalizedRoot,
               Qt::CaseInsensitive)
            == 0
        || normalizedCandidate.startsWith(
            normalizedRoot
                + QLatin1Char('/'),
            Qt::CaseInsensitive);
}

QStringList userDataRoots()
{
    QStringList roots;
    for (const auto location : {
             QStandardPaths::
                 AppDataLocation,
             QStandardPaths::
                 AppLocalDataLocation,
             QStandardPaths::
                 AppConfigLocation,
         }) {
        const QString value =
            QStandardPaths::
                writableLocation(location);
        if (!value.isEmpty()) {
            roots.append(
                normalizedPath(value));
        }
    }
    roots.removeDuplicates();
    return roots;
}

QString knownFolderPath(
    REFKNOWNFOLDERID folderId)
{
    PWSTR rawPath = nullptr;
    const HRESULT result =
        SHGetKnownFolderPath(
            folderId,
            KF_FLAG_DEFAULT,
            nullptr,
            &rawPath);
    if (FAILED(result)
        || rawPath == nullptr) {
        if (rawPath != nullptr) {
            CoTaskMemFree(rawPath);
        }
        return {};
    }

    const QString path =
        normalizedPath(
            QString::fromWCharArray(
                rawPath));
    CoTaskMemFree(rawPath);
    return path;
}

Result<void> validatePath(
    QStringView field,
    QStringView value)
{
    if (!isCleanAbsoluteLocalPath(value)) {
        return invalidRequestValue(
            field,
            QStringLiteral(
                "The update install request contains an unsafe path."));
    }
    return Result<void>::success();
}

Result<void> validateStringFields(
    const QJsonObject& object)
{
    for (const QString& field : {
             QStringLiteral("requestId"),
             QStringLiteral(
                 "installerPath"),
             QStringLiteral(
                 "expectedSha256"),
             QStringLiteral(
                 "expectedVersion"),
             QStringLiteral(
                 "installRoot"),
             QStringLiteral(
                 "rollbackRoot"),
             QStringLiteral(
                 "uninstallRegistryKey"),
             QStringLiteral(
                 "startMenuShortcut"),
             QStringLiteral(
                 "acknowledgementEvent"),
         }) {
        if (!object.value(field)
                 .isString()) {
            return invalidRequestValue(
                field,
                QStringLiteral(
                    "The update install request contains an invalid field type."));
        }
    }
    return Result<void>::success();
}

} // namespace

Result<UpdateInstallRequest>
UpdateInstallRequest::decode(
    QByteArrayView bytes)
{
    if (bytes.isEmpty()
        || bytes.size()
            > kMaximumRequestBytes) {
        return invalidRequest(
            QStringLiteral(
                "The update install request has an invalid size."));
    }

    JsonDuplicateKeyScanner scanner(bytes);
    if (!scanner.scan()
        || scanner.hasDuplicate()) {
        return invalidRequest(
            QStringLiteral(
                "The update install request could not be decoded."));
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            QByteArray(
                bytes.data(),
                bytes.size()),
            &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return invalidRequest(
            QStringLiteral(
                "The update install request could not be decoded."));
    }

    const QJsonObject object =
        document.object();
    if (!hasExactFields(object)) {
        return invalidRequest(
            QStringLiteral(
                "The update install request fields are invalid."));
    }
    const auto strings =
        validateStringFields(object);
    if (!strings.hasValue()) {
        return Result<
            UpdateInstallRequest>::failure(
                strings.error());
    }

    const auto expectedSize =
        integerValue(
            object.value(
                QStringLiteral(
                    "expectedSize")));
    const auto expectedBuild =
        integerValue(
            object.value(
                QStringLiteral(
                    "expectedBuild")));
    const auto parentProcessId =
        integerValue(
            object.value(
                QStringLiteral(
                    "parentProcessId")));
    if (!expectedSize.has_value()
        || !expectedBuild.has_value()
        || !parentProcessId.has_value()
        || *parentProcessId
            > std::numeric_limits<
                quint32>::max()) {
        return invalidRequest(
            QStringLiteral(
                "The update install request contains an invalid integer field."));
    }

    UpdateInstallRequest request;
    request.requestId =
        object.value(
                  QStringLiteral(
                      "requestId"))
            .toString();
    request.installerPath =
        normalizedPath(
            object.value(
                      QStringLiteral(
                          "installerPath"))
                .toString());
    request.expectedSha256 =
        object.value(
                  QStringLiteral(
                      "expectedSha256"))
            .toString()
            .toLower();
    request.expectedSize =
        *expectedSize;
    request.expectedVersion =
        object.value(
                  QStringLiteral(
                      "expectedVersion"))
            .toString();
    request.expectedBuild =
        *expectedBuild;
    request.installRoot =
        normalizedPath(
            object.value(
                      QStringLiteral(
                          "installRoot"))
                .toString());
    request.rollbackRoot =
        normalizedPath(
            object.value(
                      QStringLiteral(
                          "rollbackRoot"))
                .toString());
    request.uninstallRegistryKey =
        object.value(
                  QStringLiteral(
                      "uninstallRegistryKey"))
            .toString();
    request.startMenuShortcut =
        normalizedPath(
            object.value(
                      QStringLiteral(
                          "startMenuShortcut"))
                .toString());
    request.acknowledgementEvent =
        object.value(
                  QStringLiteral(
                      "acknowledgementEvent"))
            .toString();
    request.parentProcessId =
        static_cast<quint32>(
            *parentProcessId);

    const auto valid =
        request.validate();
    if (!valid.hasValue()) {
        return Result<
            UpdateInstallRequest>::failure(
                valid.error());
    }
    return Result<
        UpdateInstallRequest>::success(
            std::move(request));
}

Result<UpdateInstallRequest>
UpdateInstallRequest::load(
    QStringView path)
{
    const auto validPath =
        validatePath(
            u"requestPath",
            path);
    if (!validPath.hasValue()) {
        return Result<
            UpdateInstallRequest>::failure(
                validPath.error());
    }

    const QFileInfo information(
        path.toString());
    if (!information.isFile()
        || information.isSymLink()
        || information.size() <= 0
        || information.size()
            > kMaximumRequestBytes) {
        return invalidRequest(
            QStringLiteral(
                "The update install request file is unavailable."));
    }

    QFile file(
        information
            .absoluteFilePath());
    if (!file.open(
            QIODevice::ReadOnly)) {
        return Result<
            UpdateInstallRequest>::failure(
                requestError(
                    QStringLiteral(
                        "update.install_request_read_failed"),
                    QStringLiteral(
                        "The update install request file could not be read."),
                    {
                        {
                            QStringLiteral(
                                "path"),
                            information
                                .absoluteFilePath(),
                        },
                    }));
    }
    return decode(file.readAll());
}

Result<void>
UpdateInstallRequest::validate() const
{
    const QUuid parsedRequestId(
        requestId);
    const QString canonicalRequestId =
        parsedRequestId.toString(
            QUuid::WithoutBraces);
    if (parsedRequestId.isNull()
        || requestId
            != canonicalRequestId) {
        return invalidRequestValue(
            u"requestId",
            QStringLiteral(
                "The update install request identifier is invalid."));
    }

    for (const auto& [field, value] :
         std::array{
             std::pair<QStringView,
                       QStringView>{
                 u"installerPath",
                 installerPath,
             },
             std::pair<QStringView,
                       QStringView>{
                 u"installRoot",
                 installRoot,
             },
             std::pair<QStringView,
                       QStringView>{
                 u"rollbackRoot",
                 rollbackRoot,
             },
             std::pair<QStringView,
                       QStringView>{
                 u"startMenuShortcut",
                 startMenuShortcut,
             },
         }) {
        const auto valid =
            validatePath(field, value);
        if (!valid.hasValue()) {
            return valid;
        }
    }

    if (!installerPath.endsWith(
            QStringLiteral(".exe"),
            Qt::CaseInsensitive)) {
        return invalidRequestValue(
            u"installerPath",
            QStringLiteral(
                "The update installer path must identify an executable."));
    }
    if (!startMenuShortcut.endsWith(
            QStringLiteral(".lnk"),
            Qt::CaseInsensitive)) {
        return invalidRequestValue(
            u"startMenuShortcut",
            QStringLiteral(
                "The update shortcut path must identify a Windows shortcut."));
    }
    if (!isAsciiHexDigest(
            expectedSha256)
        || expectedSha256
            != expectedSha256.toLower()) {
        return invalidRequestValue(
            u"expectedSha256",
            QStringLiteral(
                "The update installer digest is invalid."));
    }
    if (expectedSize <= 0
        || expectedSize
            > kMaximumInstallerBytes) {
        return invalidRequestValue(
            u"expectedSize",
            QStringLiteral(
                "The update installer size is invalid."));
    }
    if (!ReleaseVersion::parse(
             expectedVersion)
             .has_value()) {
        return invalidRequestValue(
            u"expectedVersion",
            QStringLiteral(
                "The update version is invalid."));
    }
    if (expectedBuild <= 0
        || expectedBuild
            > std::numeric_limits<
                quint16>::max()) {
        return invalidRequestValue(
            u"expectedBuild",
            QStringLiteral(
                "The update build is invalid."));
    }
    if (parentProcessId == 0) {
        return invalidRequestValue(
            u"parentProcessId",
            QStringLiteral(
                "The update parent process identifier is invalid."));
    }

    if (sameWindowsPath(
            installRoot,
            rollbackRoot)) {
        return invalidRequestValue(
            u"rollbackRoot",
            QStringLiteral(
                "The update rollback directory must be distinct from the install directory."));
    }
    if (!sameWindowsPath(
            installRoot,
            expectedInstallRoot())) {
        return invalidRequestValue(
            u"installRoot",
            QStringLiteral(
                "The update install directory is not the per-user Codex Companion installation."));
    }
    const QString installParent =
        normalizedPath(
            QFileInfo(installRoot)
                .absolutePath());
    const QString rollbackParent =
        normalizedPath(
            QFileInfo(rollbackRoot)
                .absolutePath());
    if (!sameWindowsPath(
            installParent,
            rollbackParent)) {
        return invalidRequestValue(
            u"rollbackRoot",
            QStringLiteral(
                "The update rollback directory must be a sibling of the install directory."));
    }
    const QString expectedRollbackName =
        QFileInfo(installRoot)
            .fileName()
        + QStringLiteral(".rollback.")
        + requestId;
    if (QFileInfo(rollbackRoot)
            .fileName()
            .compare(
                expectedRollbackName,
                Qt::CaseInsensitive)
        != 0) {
        return invalidRequestValue(
            u"rollbackRoot",
            QStringLiteral(
                "The update rollback directory name is invalid."));
    }

    for (const QString& dataRoot :
         userDataRoots()) {
        if (isWithinTree(
                installRoot,
                dataRoot)
            || isWithinTree(
                dataRoot,
                installRoot)
            || isWithinTree(
                rollbackRoot,
                dataRoot)
            || isWithinTree(
                dataRoot,
                rollbackRoot)) {
            return invalidRequestValue(
                u"installRoot",
                QStringLiteral(
                    "The update install directories overlap Companion user data."));
        }
    }

    if (uninstallRegistryKey
            .compare(
                expectedUninstallRegistryKey(),
                Qt::CaseInsensitive)
        != 0) {
        return invalidRequestValue(
            u"uninstallRegistryKey",
            QStringLiteral(
                "The update uninstall registry key is invalid."));
    }
    if (!sameWindowsPath(
            startMenuShortcut,
            expectedStartMenuShortcut())) {
        return invalidRequestValue(
            u"startMenuShortcut",
            QStringLiteral(
                "The update Start Menu shortcut path is invalid."));
    }
    if (acknowledgementEvent
            != acknowledgementEventFor(
                requestId)) {
        return invalidRequestValue(
            u"acknowledgementEvent",
            QStringLiteral(
                "The update acknowledgement event name is invalid."));
    }

    return Result<void>::success();
}

Result<void>
UpdateInstallRequest::writeAtomically(
    QStringView path) const
{
    const auto valid = validate();
    if (!valid.hasValue()) {
        return valid;
    }
    const auto validPath =
        validatePath(
            u"requestPath",
            path);
    if (!validPath.hasValue()) {
        return validPath;
    }

    const QFileInfo information(
        path.toString());
    QDir directory =
        information.dir();
    if (!directory.exists()
        && !QDir().mkpath(
            directory.absolutePath())) {
        return Result<void>::failure(
            requestError(
                QStringLiteral(
                    "update.install_request_write_failed"),
                QStringLiteral(
                    "The update install request directory could not be created."),
                {
                    {
                        QStringLiteral("path"),
                        directory.absolutePath(),
                    },
                }));
    }

    QSaveFile file(
        information
            .absoluteFilePath());
    file.setDirectWriteFallback(false);
    const QByteArray bytes = encode();
    if (!file.open(
            QIODevice::WriteOnly)
        || file.write(bytes)
            != bytes.size()
        || !file.commit()) {
        file.cancelWriting();
        return Result<void>::failure(
            requestError(
                QStringLiteral(
                    "update.install_request_write_failed"),
                QStringLiteral(
                    "The update install request file could not be written."),
                {
                    {
                        QStringLiteral("path"),
                        information
                            .absoluteFilePath(),
                    },
                }));
    }
    return Result<void>::success();
}

QByteArray UpdateInstallRequest::encode()
    const
{
    QJsonObject object;
    object.insert(
        QStringLiteral("requestId"),
        requestId);
    object.insert(
        QStringLiteral("installerPath"),
        normalizedPath(
            installerPath));
    object.insert(
        QStringLiteral("expectedSha256"),
        expectedSha256);
    object.insert(
        QStringLiteral("expectedSize"),
        expectedSize);
    object.insert(
        QStringLiteral("expectedVersion"),
        expectedVersion);
    object.insert(
        QStringLiteral("expectedBuild"),
        expectedBuild);
    object.insert(
        QStringLiteral("installRoot"),
        normalizedPath(installRoot));
    object.insert(
        QStringLiteral("rollbackRoot"),
        normalizedPath(rollbackRoot));
    object.insert(
        QStringLiteral(
            "uninstallRegistryKey"),
        uninstallRegistryKey);
    object.insert(
        QStringLiteral(
            "startMenuShortcut"),
        normalizedPath(
            startMenuShortcut));
    object.insert(
        QStringLiteral(
            "acknowledgementEvent"),
        acknowledgementEvent);
    object.insert(
        QStringLiteral(
            "parentProcessId"),
        static_cast<qint64>(
            parentProcessId));
    return QJsonDocument(
               std::move(object))
        .toJson(
            QJsonDocument::Compact);
}

QString UpdateInstallRequest::
expectedInstallRoot()
{
    const QString localAppData =
        knownFolderPath(
            FOLDERID_LocalAppData);
    if (localAppData.isEmpty()) {
        return {};
    }
    return normalizedPath(
        QDir(localAppData).filePath(
            QStringLiteral(
                "Programs/Codex Companion")));
}

QString UpdateInstallRequest::
expectedUninstallRegistryKey()
{
    return QStringLiteral(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        "{9B3C42CB-4B7F-4A08-B675-071708948C88}_is1");
}

QString UpdateInstallRequest::
expectedStartMenuShortcut()
{
    const QString programs =
        knownFolderPath(
            FOLDERID_Programs);
    if (programs.isEmpty()) {
        return {};
    }
    return normalizedPath(
        QDir(programs).filePath(
            QStringLiteral(
                "Codex Companion/"
                "Codex Companion.lnk")));
}

QString UpdateInstallRequest::
acknowledgementEventFor(
    QStringView requestId)
{
    return QStringLiteral(
               "Local\\CodexCompanion."
               "UpdateAck.")
        + requestId;
}

QString UpdateInstallRequest::
helperReadyEventFor(
    QStringView requestId)
{
    return QStringLiteral(
               "Local\\CodexCompanion."
               "UpdateHelperReady.")
        + requestId;
}

} // namespace companion
