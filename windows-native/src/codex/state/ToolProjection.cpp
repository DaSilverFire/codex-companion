#include "codex/state/ToolProjection.h"

#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <optional>
#include <utility>

namespace companion {

namespace {

inline constexpr qsizetype kMaximumDetailCharacters = 2000;

std::optional<QString> nonempty(QString value)
{
    value = value.trimmed();
    return value.isEmpty()
        ? std::nullopt
        : std::optional<QString>(std::move(value));
}

std::optional<QJsonObject> jsonObject(const QString& source)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        source.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

std::optional<QString> firstStringValue(
    const QStringList& keys,
    const QJsonObject& object)
{
    for (const QString& key : keys) {
        if (!object.value(key).isString()) {
            continue;
        }
        if (const auto value = nonempty(object.value(key).toString());
            value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

QString decodedLiteral(QString value, QChar quote)
{
    if (quote == QLatin1Char('"')) {
        const QByteArray encoded =
            QByteArray("[\"") + value.toUtf8() + QByteArray("\"]");
        QJsonParseError error;
        const QJsonDocument document =
            QJsonDocument::fromJson(encoded, &error);
        if (error.error == QJsonParseError::NoError &&
            document.isArray() &&
            document.array().size() == 1 &&
            document.array().first().isString()) {
            return document.array().first().toString();
        }
    }

    value.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
    value.replace(QStringLiteral("\\t"), QStringLiteral("\t"));
    value.replace(
        QStringLiteral("\\") + quote,
        QString(quote));
    value.replace(
        QStringLiteral("\\\\"),
        QStringLiteral("\\"));
    return value;
}

std::optional<QString> firstStringLiteral(
    const QStringList& keys,
    const QString& source)
{
    for (const QString& key : keys) {
        const QRegularExpression expression(
            QStringLiteral("\\b%1\\s*:\\s*")
                .arg(QRegularExpression::escape(key)));
        const QRegularExpressionMatch match =
            expression.match(source);
        if (!match.hasMatch()) {
            continue;
        }

        const qsizetype start = match.capturedEnd();
        if (start >= source.size()) {
            continue;
        }
        const QChar quote = source.at(start);
        if (quote != QLatin1Char('"') &&
            quote != QLatin1Char('\'') &&
            quote != QLatin1Char('`')) {
            continue;
        }

        bool escaped = false;
        for (qsizetype index = start + 1;
             index < source.size();
             ++index) {
            const QChar character = source.at(index);
            if (escaped) {
                escaped = false;
            } else if (character == QLatin1Char('\\')) {
                escaped = true;
            } else if (character == quote) {
                const QString raw = source.mid(
                    start + 1, index - start - 1);
                return nonempty(decodedLiteral(raw, quote));
            }
        }
    }
    return std::nullopt;
}

QString boundedDetail(const QString& value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    QTextBoundaryFinder finder(
        QTextBoundaryFinder::Grapheme, trimmed);
    QVector<qsizetype> boundaries{0};
    finder.toStart();
    while (true) {
        const qsizetype boundary = finder.toNextBoundary();
        if (boundary < 0) {
            break;
        }
        boundaries.append(boundary);
    }

    if (boundaries.size() - 1 <= kMaximumDetailCharacters) {
        return trimmed;
    }
    return trimmed.left(
        boundaries.at(kMaximumDetailCharacters - 3)) +
        QStringLiteral("...");
}

std::optional<QString> boundedOptional(
    const std::optional<QString>& value)
{
    if (!value.has_value()) {
        return std::nullopt;
    }
    const QString bounded = boundedDetail(*value);
    return bounded.isEmpty()
        ? std::nullopt
        : std::optional<QString>(bounded);
}

QString leafName(const QString& normalizedName)
{
    const QStringList components =
        normalizedName.split(QStringLiteral("__"));
    return components.isEmpty()
        ? normalizedName
        : components.last();
}

bool matchesCommand(
    const QString& command,
    const QString& pattern)
{
    return QRegularExpression(pattern).match(command).hasMatch();
}

std::optional<QString> commandText(
    const std::optional<QString>& input)
{
    if (!input.has_value()) {
        return std::nullopt;
    }
    if (const auto object = jsonObject(*input);
        object.has_value()) {
        if (const auto command = firstStringValue(
                {QStringLiteral("cmd")}, *object);
            command.has_value()) {
            return command;
        }
    }
    return firstStringLiteral(
        {QStringLiteral("cmd")}, *input);
}

std::optional<QString> commandTitle(
    const std::optional<QString>& input)
{
    const auto command = commandText(input);
    if (!command.has_value()) {
        return std::nullopt;
    }

    const QString normalized =
        command->trimmed().toLower();
    if (normalized.contains(QLatin1Char('\n')) ||
        normalized.contains(QStringLiteral(" && ")) ||
        normalized.contains(QStringLiteral(" || ")) ||
        normalized.contains(QLatin1Char(';')) ||
        normalized.contains(QStringLiteral(" | "))) {
        return std::nullopt;
    }

    static const QRegularExpression environmentPrefix(
        QStringLiteral(
            "^env(?:\\s+[A-Za-z_][A-Za-z0-9_]*="
            "(?:'[^']*'|\"[^\"]*\"|\\S+))*\\s+"));
    QString unwrapped = normalized;
    unwrapped.remove(environmentPrefix);

    const bool readsFiles =
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:/usr/bin/|/bin/)?"
                "(?:cat|sed|head|tail|nl|wc|stat|file)\\b")) ||
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:/usr/bin/)?defaults\\s+read\\b")) ||
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:/usr/bin/)?plutil\\s+-p\\b")) ||
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:get-content|gc|type)\\b"));
    if (readsFiles) {
        return QStringLiteral("Read files");
    }

    const bool searchesFiles =
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:/usr/bin/|/bin/)?"
                "(?:rg|grep|find|fd|ls)\\b")) ||
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:/usr/bin/)?git\\s+(?:grep|ls-files)\\b")) ||
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:get-childitem|gci|dir|select-string)\\b"));
    if (searchesFiles) {
        return QStringLiteral("Searched files");
    }

    const bool testsApp =
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:/usr/bin/)?xcodebuild\\b.*\\btest\\b")) ||
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:/usr/bin/)?swift\\s+test\\b")) ||
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:/usr/bin/)?"
                "(?:pytest|cargo\\s+test|go\\s+test|ctest)\\b")) ||
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:/usr/bin/)?python(?:3)?\\s+-m\\s+pytest\\b")) ||
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:npm|pnpm|yarn)\\s+(?:run\\s+)?test\\b")) ||
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:dotnet\\s+test|vstest\\.console)\\b"));
    if (testsApp) {
        return QStringLiteral("Tested the app");
    }

    const bool buildsApp =
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:/usr/bin/)?xcodebuild\\b")) ||
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:/usr/bin/)?swift\\s+build\\b")) ||
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:npm|pnpm|yarn)\\s+(?:run\\s+)?build\\b")) ||
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:/usr/bin/)?cargo\\s+build\\b")) ||
        matchesCommand(
            unwrapped,
            QStringLiteral(
                "^(?:cmake\\s+--build|dotnet\\s+build|msbuild)\\b"));
    if (buildsApp) {
        return QStringLiteral("Built the app");
    }

    return std::nullopt;
}

QVector<QString> shellWords(const QString& source)
{
    QVector<QString> words;
    QString current;
    std::optional<QChar> quote;
    bool escaped = false;

    const auto finishWord = [&words, &current]() {
        if (!current.isEmpty()) {
            words.append(current);
            current.clear();
        }
    };

    for (qsizetype index = 0; index < source.size(); ++index) {
        const QChar character = source.at(index);
        if (escaped) {
            current.append(character);
            escaped = false;
            continue;
        }
        if (character == QLatin1Char('\\') &&
            quote != QLatin1Char('\'')) {
            const bool hasNext = index + 1 < source.size();
            const QChar next = hasNext
                ? source.at(index + 1)
                : QChar();
            const bool escapesShellCharacter =
                hasNext &&
                (next.isSpace() ||
                 next == QLatin1Char('\\') ||
                 next == QLatin1Char('"') ||
                 next == QLatin1Char('\''));
            if (escapesShellCharacter) {
                escaped = true;
            } else {
                current.append(character);
            }
            continue;
        }
        if (quote.has_value()) {
            if (character == *quote) {
                quote = std::nullopt;
            } else {
                current.append(character);
            }
            continue;
        }
        if (character == QLatin1Char('"') ||
            character == QLatin1Char('\'')) {
            quote = character;
        } else if (character.isSpace()) {
            finishWord();
        } else {
            current.append(character);
        }
    }
    if (escaped) {
        current.append(QLatin1Char('\\'));
    }
    finishWord();
    return words;
}

QVector<QString> positionalArguments(
    const QVector<QString>& arguments,
    const QSet<QString>& optionsWithValues)
{
    QVector<QString> values;
    qsizetype index = 0;
    bool acceptsOptions = true;
    while (index < arguments.size()) {
        const QString& argument = arguments.at(index);
        if (acceptsOptions &&
            argument == QStringLiteral("--")) {
            acceptsOptions = false;
            ++index;
            continue;
        }
        if (acceptsOptions &&
            optionsWithValues.contains(argument)) {
            index += 2;
            continue;
        }
        if (acceptsOptions &&
            argument.startsWith(QLatin1Char('-'))) {
            ++index;
            continue;
        }
        values.append(argument);
        ++index;
    }
    return values;
}

QVector<QString> sedInputPaths(
    const QVector<QString>& arguments)
{
    QVector<QString> values;
    qsizetype index = 0;
    bool hasExplicitProgram = false;
    bool consumedImplicitProgram = false;
    while (index < arguments.size()) {
        const QString& argument = arguments.at(index);
        if (argument == QStringLiteral("-e") ||
            argument == QStringLiteral("--expression")) {
            hasExplicitProgram = true;
            index += 2;
            continue;
        }
        if (argument.startsWith(QStringLiteral("-e")) &&
            argument.size() > 2) {
            hasExplicitProgram = true;
            ++index;
            continue;
        }
        if (argument == QStringLiteral("-f") ||
            argument == QStringLiteral("--file")) {
            if (index + 1 < arguments.size()) {
                values.append(arguments.at(index + 1));
            }
            hasExplicitProgram = true;
            index += 2;
            continue;
        }
        if (argument.startsWith(QLatin1Char('-'))) {
            ++index;
            continue;
        }
        if (!hasExplicitProgram &&
            !consumedImplicitProgram) {
            consumedImplicitProgram = true;
        } else {
            values.append(argument);
        }
        ++index;
    }
    return values;
}

std::optional<QVector<QString>> readFilePaths(
    const QString& command)
{
    const QVector<QString> words = shellWords(command);
    if (words.isEmpty()) {
        return std::nullopt;
    }

    qsizetype commandIndex = 0;
    if (QFileInfo(
            QDir::fromNativeSeparators(words.first()))
            .fileName()
            .compare(QStringLiteral("env"), Qt::CaseInsensitive) == 0) {
        ++commandIndex;
        static const QRegularExpression assignment(
            QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*="));
        while (commandIndex < words.size() &&
               assignment.match(words.at(commandIndex)).hasMatch()) {
            ++commandIndex;
        }
    }
    if (commandIndex >= words.size()) {
        return std::nullopt;
    }

    const QString executable = QFileInfo(
        QDir::fromNativeSeparators(words.at(commandIndex)))
        .fileName()
        .toLower();
    const QVector<QString> arguments =
        words.mid(commandIndex + 1);
    QVector<QString> candidates;

    if (executable == QStringLiteral("cat") ||
        executable == QStringLiteral("nl") ||
        executable == QStringLiteral("wc") ||
        executable == QStringLiteral("plutil")) {
        candidates = positionalArguments(
            arguments,
            executable == QStringLiteral("plutil")
                ? QSet<QString>{}
                : QSet<QString>{QStringLiteral("-w")});
    } else if (
        executable == QStringLiteral("head") ||
        executable == QStringLiteral("tail")) {
        candidates = positionalArguments(
            arguments,
            {
                QStringLiteral("-n"),
                QStringLiteral("--lines"),
                QStringLiteral("-c"),
                QStringLiteral("--bytes"),
            });
    } else if (executable == QStringLiteral("stat")) {
        candidates = positionalArguments(
            arguments,
            {
                QStringLiteral("-f"),
                QStringLiteral("--format"),
                QStringLiteral("-t"),
            });
    } else if (executable == QStringLiteral("file")) {
        candidates = positionalArguments(
            arguments,
            {
                QStringLiteral("-e"),
                QStringLiteral("--exclude"),
                QStringLiteral("-m"),
                QStringLiteral("--magic-file"),
            });
    } else if (executable == QStringLiteral("sed")) {
        candidates = sedInputPaths(arguments);
    } else if (
        executable == QStringLiteral("get-content") ||
        executable == QStringLiteral("gc") ||
        executable == QStringLiteral("type")) {
        candidates = positionalArguments(
            arguments,
            {
                QStringLiteral("-Path"),
                QStringLiteral("-LiteralPath"),
                QStringLiteral("-TotalCount"),
                QStringLiteral("-Tail"),
            });
    } else {
        return std::nullopt;
    }

    QVector<QString> paths;
    for (QString candidate : candidates) {
        candidate = candidate.trimmed();
        if (candidate.isEmpty() ||
            candidate == QStringLiteral("-") ||
            candidate.startsWith(QLatin1Char('>')) ||
            candidate.startsWith(QLatin1Char('<')) ||
            candidate.contains(QStringLiteral(">/dev/null")) ||
            paths.contains(candidate)) {
            continue;
        }
        paths.append(candidate);
    }
    return paths.isEmpty()
        ? std::nullopt
        : std::optional<QVector<QString>>(paths);
}

QString humanReadableIdentifier(QString value)
{
    value.replace(QLatin1Char('_'), QLatin1Char(' '));
    value.replace(QLatin1Char('-'), QLatin1Char(' '));
    const QStringList words =
        value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList result;
    result.reserve(words.size());
    for (const QString& word : words) {
        result.append(
            word.left(1).toUpper() + word.mid(1));
    }
    return result.join(QLatin1Char(' '));
}

std::optional<QString> integrationDetail(
    const QString& name,
    const std::optional<QString>& server,
    const std::optional<QString>& semanticValue)
{
    const QStringList components =
        name.split(QStringLiteral("__"));
    const std::optional<QString> inferredServer =
        components.size() >= 3
            ? std::optional<QString>(components.at(1))
            : std::nullopt;
    const QString rawServer = server.value_or(
        inferredServer.value_or(QString()));
    const auto serverLabel = nonempty(rawServer).has_value()
        ? std::optional<QString>(
              humanReadableIdentifier(rawServer))
        : std::nullopt;
    const auto toolLabel =
        components.isEmpty()
            ? std::nullopt
            : nonempty(
                  humanReadableIdentifier(components.last()));

    QStringList parts;
    const QVector<std::optional<QString>> candidates{
        serverLabel,
        semanticValue,
        semanticValue.has_value()
            ? std::nullopt
            : toolLabel,
    };
    for (const auto& candidate : candidates) {
        if (candidate.has_value() &&
            !parts.contains(*candidate)) {
            parts.append(*candidate);
        }
    }
    return parts.isEmpty()
        ? std::nullopt
        : std::optional<QString>(
              parts.join(QStringLiteral(" - ")));
}

std::optional<QString> safePlainTextDetail(
    const QString& input)
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty() ||
        trimmed.startsWith(QLatin1Char('{')) ||
        trimmed.startsWith(QLatin1Char('[')) ||
        trimmed.startsWith(QStringLiteral("const ")) ||
        trimmed.startsWith(QStringLiteral("let ")) ||
        trimmed.startsWith(QStringLiteral("var ")) ||
        trimmed.startsWith(QStringLiteral("await ")) ||
        trimmed.contains(QStringLiteral("tools."))) {
        return std::nullopt;
    }
    return trimmed;
}

std::optional<QString> nestedToolName(
    const std::optional<QString>& source)
{
    if (!source.has_value()) {
        return std::nullopt;
    }
    static const QRegularExpression expression(
        QStringLiteral("tools\\.([A-Za-z0-9_]+)\\s*\\("));
    const QRegularExpressionMatch match =
        expression.match(*source);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    return match.captured(1).toLower();
}

bool looksLikeToolInventoryPayload(
    const std::optional<QString>& source)
{
    return source.has_value() &&
        source->contains(
            QStringLiteral("all_tools"),
            Qt::CaseInsensitive);
}

std::optional<QString> editedFilePathsFromInput(
    const QString& input)
{
    QVector<QString> candidates{input};
    if (const auto object = jsonObject(input);
        object.has_value()) {
        if (const auto payload = firstStringValue(
                {
                    QStringLiteral("input"),
                    QStringLiteral("patch"),
                },
                *object);
            payload.has_value()) {
            candidates.append(*payload);
        }
    }
    if (const auto payload = firstStringLiteral(
            {
                QStringLiteral("input"),
                QStringLiteral("patch"),
            },
            input);
        payload.has_value()) {
        candidates.append(*payload);
    }

    static const QRegularExpression expression(
        QStringLiteral(
            "^\\*\\*\\* (?:Update|Add|Delete) File:\\s*(.+?)\\s*$"),
        QRegularExpression::MultilineOption);
    QStringList paths;
    for (const QString& candidate : candidates) {
        QRegularExpressionMatchIterator matches =
            expression.globalMatch(candidate);
        while (matches.hasNext()) {
            const QString path =
                matches.next().captured(1).trimmed();
            if (!path.isEmpty() &&
                !paths.contains(path)) {
                paths.append(path);
            }
        }
    }
    return paths.isEmpty()
        ? std::nullopt
        : std::optional<QString>(
              paths.join(QLatin1Char('\n')));
}

std::optional<QString> agentDetail(
    const QString& input,
    bool isWrapper)
{
    if (jsonObject(input).has_value()) {
        return input;
    }
    if (!isWrapper) {
        return input;
    }

    QJsonObject object;
    for (const QString& key : {
             QStringLiteral("target"),
             QStringLiteral("agent_type"),
             QStringLiteral("message"),
             QStringLiteral("prompt"),
             QStringLiteral("title"),
         }) {
        if (const auto value = firstStringLiteral({key}, input);
            value.has_value()) {
            object.insert(key, *value);
        }
    }
    return object.isEmpty()
        ? std::nullopt
        : std::optional<QString>(
              QString::fromUtf8(
                  QJsonDocument(object).toJson(
                      QJsonDocument::Compact)));
}

std::optional<QString> appInspectionDetail(
    const QString& input)
{
    if (const auto object = jsonObject(input);
        object.has_value()) {
        if (const auto value = firstStringValue(
                {
                    QStringLiteral("title"),
                    QStringLiteral("app"),
                },
                *object);
            value.has_value()) {
            return value;
        }
    }
    return firstStringLiteral(
        {
            QStringLiteral("title"),
            QStringLiteral("app"),
        },
        input);
}

QString semanticTitle(
    const QString& name,
    const std::optional<QString>& input,
    const std::optional<QString>& server)
{
    const QString normalized = name.toLower();
    const QString leaf = leafName(normalized);
    const std::optional<QString> normalizedServer =
        server.has_value()
            ? std::optional<QString>(server->toLower())
            : std::nullopt;

    if (leaf == QStringLiteral("tool_search") ||
        leaf == QStringLiteral("load_workspace_dependencies") ||
        leaf == QStringLiteral("list_mcp_resources") ||
        leaf == QStringLiteral("list_mcp_resource_templates")) {
        return QStringLiteral("Loaded tools");
    }

    if (normalizedServer.has_value()) {
        if (*normalizedServer == QStringLiteral("computer-use") ||
            *normalizedServer == QStringLiteral("node_repl")) {
            return QStringLiteral("Inspected an app");
        }
        if (*normalizedServer == QStringLiteral("xcodebuildmcp")) {
            if (leaf.contains(QStringLiteral("screenshot"))) {
                return QStringLiteral("Viewed an image");
            }
            if (leaf.contains(QStringLiteral("snapshot")) ||
                leaf.contains(QStringLiteral("inspect"))) {
                return QStringLiteral("Inspected an app");
            }
            if (leaf.contains(QStringLiteral("build")) ||
                leaf.contains(QStringLiteral("launch")) ||
                leaf.contains(QStringLiteral("test"))) {
                return QStringLiteral("Tested the app");
            }
            return QStringLiteral("Used an integration");
        }
        return QStringLiteral("Used an integration");
    }

    if (normalized.startsWith(
            QStringLiteral("mcp__node_repl__")) ||
        normalized.startsWith(
            QStringLiteral("mcp__computer_use__")) ||
        leaf == QStringLiteral("computer_use") ||
        leaf == QStringLiteral("js") ||
        QVector<QString>{
            QStringLiteral("click"),
            QStringLiteral("get_app_state"),
            QStringLiteral("list_apps"),
            QStringLiteral("set_value"),
            QStringLiteral("type_text"),
        }.contains(leaf)) {
        return QStringLiteral("Inspected an app");
    }
    if (QVector<QString>{
            QStringLiteral("exec"),
            QStringLiteral("exec_command"),
            QStringLiteral("write_stdin"),
        }.contains(leaf)) {
        return commandTitle(input).value_or(
            QStringLiteral("Ran a command"));
    }
    if (leaf == QStringLiteral("view_image") ||
        leaf.contains(QStringLiteral("screenshot"))) {
        return QStringLiteral("Viewed an image");
    }
    if (leaf == QStringLiteral("apply_patch") ||
        leaf.contains(QStringLiteral("patch"))) {
        return QStringLiteral("Edited files");
    }
    if (leaf == QStringLiteral("find") ||
        leaf == QStringLiteral("rg") ||
        leaf.contains(QStringLiteral("search"))) {
        return QStringLiteral("Searched files");
    }
    if (leaf == QStringLiteral("spawn_agent") ||
        leaf == QStringLiteral("send_input")) {
        return QStringLiteral("Messaged an agent");
    }
    if (leaf == QStringLiteral("close_agent") ||
        leaf == QStringLiteral("resume_agent")) {
        return QStringLiteral("Managed an agent");
    }
    if (leaf == QStringLiteral("wait_agent") ||
        leaf == QStringLiteral("wait")) {
        return QStringLiteral("Wait");
    }
    if (leaf.contains(QStringLiteral("read")) &&
        leaf.contains(QStringLiteral("file"))) {
        return QStringLiteral("Read a file");
    }
    if (leaf == QStringLiteral("open") &&
        normalized.contains(QStringLiteral("browser"))) {
        return QStringLiteral("Opened a link");
    }
    if (leaf == QStringLiteral("update_plan")) {
        return QStringLiteral("Updated progress");
    }
    if (leaf == QStringLiteral("get_goal")) {
        return QStringLiteral("Checked the goal");
    }
    if (leaf == QStringLiteral("create_goal") ||
        leaf == QStringLiteral("update_goal")) {
        return QStringLiteral("Updated the goal");
    }
    if (normalized.contains(QStringLiteral("image_gen")) ||
        leaf == QStringLiteral("imagegen")) {
        return QStringLiteral("Generated an image");
    }
    if (normalized.startsWith(QStringLiteral("web__")) ||
        leaf == QStringLiteral("web_run") ||
        leaf == QStringLiteral("run") &&
            normalized.contains(QStringLiteral("web"))) {
        return QStringLiteral("Searched the web");
    }
    if (leaf == QStringLiteral("request_user_input")) {
        return QStringLiteral("Asked a question");
    }
    if (leaf.contains(QStringLiteral("automation"))) {
        return QStringLiteral("Updated an automation");
    }
    if (normalized.startsWith(QStringLiteral("mcp__")) ||
        leaf == QStringLiteral("read_mcp_resource")) {
        return QStringLiteral("Used an integration");
    }
    return QStringLiteral("Used a tool");
}

std::optional<QString> semanticDetail(
    const QString& title,
    const QString& name,
    const std::optional<QString>& input,
    const std::optional<QString>& server,
    bool isWrapper)
{
    if (!input.has_value()) {
        return std::nullopt;
    }
    const auto trimmedInput = nonempty(*input);
    if (!trimmedInput.has_value()) {
        return std::nullopt;
    }

    if (title == QStringLiteral("Messaged an agent") ||
        title == QStringLiteral("Managed an agent")) {
        return boundedOptional(
            agentDetail(*trimmedInput, isWrapper));
    }
    if (title == QStringLiteral("Edited files")) {
        return boundedOptional(
            editedFilePathsFromInput(*trimmedInput));
    }
    if (title == QStringLiteral("Inspected an app")) {
        return boundedOptional(
            appInspectionDetail(*trimmedInput));
    }
    if (title == QStringLiteral("Read files")) {
        const auto command = commandText(input);
        if (command.has_value()) {
            const auto paths = readFilePaths(*command);
            if (paths.has_value()) {
                return boundedOptional(
                    paths->isEmpty()
                        ? std::nullopt
                        : std::optional<QString>(
                              QStringList(
                                  paths->cbegin(),
                                  paths->cend())
                                  .join(QLatin1Char('\n'))));
            }
        }
    }

    QStringList keys;
    if (title == QStringLiteral("Ran a command")) {
        keys = {QStringLiteral("cmd")};
    } else if (
        title == QStringLiteral("Read a file") ||
        title == QStringLiteral("Read files")) {
        keys = {
            QStringLiteral("cmd"),
            QStringLiteral("path"),
            QStringLiteral("file"),
            QStringLiteral("workdir"),
        };
    } else if (title == QStringLiteral("Edited files")) {
        keys = {
            QStringLiteral("path"),
            QStringLiteral("file"),
        };
    } else if (title == QStringLiteral("Searched files")) {
        keys = {
            QStringLiteral("cmd"),
            QStringLiteral("q"),
            QStringLiteral("query"),
            QStringLiteral("pattern"),
        };
    } else if (title == QStringLiteral("Searched the web")) {
        keys = {
            QStringLiteral("q"),
            QStringLiteral("query"),
            QStringLiteral("pattern"),
        };
    } else if (title == QStringLiteral("Viewed an image")) {
        keys = {
            QStringLiteral("path"),
            QStringLiteral("url"),
            QStringLiteral("title"),
        };
    } else if (title == QStringLiteral("Inspected an app")) {
        keys = {
            QStringLiteral("title"),
            QStringLiteral("app"),
        };
    } else if (
        title == QStringLiteral("Built the app") ||
        title == QStringLiteral("Tested the app")) {
        keys = {
            QStringLiteral("cmd"),
            QStringLiteral("title"),
            QStringLiteral("scheme"),
            QStringLiteral("app"),
        };
    } else if (title == QStringLiteral("Opened a link")) {
        keys = {
            QStringLiteral("url"),
            QStringLiteral("ref_id"),
        };
    } else if (title == QStringLiteral("Updated progress")) {
        keys = {QStringLiteral("explanation")};
    } else if (
        title == QStringLiteral("Checked the goal") ||
        title == QStringLiteral("Updated the goal")) {
        keys = {
            QStringLiteral("objective"),
            QStringLiteral("status"),
        };
    } else if (title == QStringLiteral("Generated an image")) {
        keys = {QStringLiteral("prompt")};
    } else if (title == QStringLiteral("Asked a question")) {
        keys = {QStringLiteral("question")};
    } else if (title == QStringLiteral("Loaded tools")) {
        keys = {
            QStringLiteral("query"),
            QStringLiteral("q"),
            QStringLiteral("name"),
            QStringLiteral("server"),
            QStringLiteral("uri"),
        };
    } else if (
        title == QStringLiteral("Used an integration") ||
        title == QStringLiteral("Updated an automation")) {
        keys = {
            QStringLiteral("title"),
            QStringLiteral("q"),
            QStringLiteral("query"),
            QStringLiteral("prompt"),
            QStringLiteral("url"),
            QStringLiteral("name"),
        };
    } else {
        keys = {
            QStringLiteral("title"),
            QStringLiteral("path"),
            QStringLiteral("file"),
            QStringLiteral("q"),
            QStringLiteral("query"),
            QStringLiteral("url"),
            QStringLiteral("name"),
        };
    }

    std::optional<QString> semanticValue;
    if (const auto object = jsonObject(*trimmedInput);
        object.has_value()) {
        semanticValue = firstStringValue(keys, *object);
    }
    if (!semanticValue.has_value()) {
        semanticValue =
            firstStringLiteral(keys, *trimmedInput);
    }

    if (title == QStringLiteral("Used an integration")) {
        return boundedOptional(integrationDetail(
            name, server, semanticValue));
    }
    if (semanticValue.has_value()) {
        return boundedOptional(semanticValue);
    }
    if (isWrapper ||
        title == QStringLiteral("Edited files")) {
        return std::nullopt;
    }
    return boundedOptional(
        safePlainTextDetail(*trimmedInput));
}

} // namespace

ToolProjection ToolProjector::project(
    QString name,
    std::optional<QString> input,
    std::optional<QString> server)
{
    const QString normalizedName =
        name.trimmed().toLower();
    const bool isExecWrapper =
        normalizedName == QStringLiteral("exec");
    const bool isJavaScriptWrapper =
        normalizedName == QStringLiteral("js") ||
        normalizedName.endsWith(QStringLiteral("__js")) ||
        normalizedName.endsWith(QStringLiteral(".js"));
    const bool isTransportWrapper =
        isExecWrapper || isJavaScriptWrapper;
    const auto nestedName =
        isExecWrapper
            ? nestedToolName(input)
            : std::nullopt;

    QString effectiveName;
    if (nestedName.has_value()) {
        effectiveName = *nestedName;
    } else if (
        isTransportWrapper &&
        looksLikeToolInventoryPayload(input)) {
        effectiveName = QStringLiteral("tool_search");
    } else {
        effectiveName = normalizedName;
    }

    const QString title =
        semanticTitle(effectiveName, input, server);
    return {
        title,
        semanticDetail(
            title,
            effectiveName,
            input,
            server,
            isTransportWrapper),
        isExecWrapper &&
            effectiveName.startsWith(QStringLiteral("mcp__")),
    };
}

std::optional<QString> ToolProjector::editedFilePathsFromChanges(
    const QJsonValue& changes)
{
    if (!changes.isObject()) {
        return std::nullopt;
    }

    QStringList paths;
    const QJsonObject object = changes.toObject();
    for (auto iterator = object.constBegin();
         iterator != object.constEnd();
         ++iterator) {
        if (const auto path = nonempty(iterator.key());
            path.has_value()) {
            paths.append(*path);
        }
    }

    QCollator collator;
    collator.setNumericMode(true);
    std::sort(
        paths.begin(),
        paths.end(),
        [&collator](
            const QString& left,
            const QString& right) {
            return collator.compare(left, right) < 0;
        });
    paths.removeDuplicates();
    return paths.isEmpty()
        ? std::nullopt
        : std::optional<QString>(
              paths.join(QLatin1Char('\n')));
}

std::optional<QString>
ToolProjector::editedFilePathsFromToolOutput(
    const QString& output)
{
    const QString trimmed = output.trimmed();
    const qsizetype marker = trimmed.indexOf(
        QStringLiteral("Updated the following files:"),
        0,
        Qt::CaseInsensitive);
    if (marker < 0) {
        return std::nullopt;
    }

    const QString suffix = trimmed.mid(
        marker +
        QStringLiteral("Updated the following files:").size());
    QStringList paths;
    const QStringList lines =
        suffix.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (QString line : lines) {
        line = line.trimmed();
        if (line.size() < 3 ||
            !QStringLiteral("MADRC?").contains(line.at(0)) ||
            !line.at(1).isSpace()) {
            continue;
        }
        const QString path = line.mid(1).trimmed();
        if (!path.isEmpty() &&
            !paths.contains(path)) {
            paths.append(path);
        }
    }
    return paths.isEmpty()
        ? std::nullopt
        : std::optional<QString>(
              paths.join(QLatin1Char('\n')));
}

TimelineStatus ToolProjector::callStatus(
    const std::optional<QString>& rawStatus)
{
    const QString normalized = rawStatus.has_value()
        ? rawStatus->toLower()
        : QString();
    if (normalized == QStringLiteral("failed") ||
        normalized == QStringLiteral("error") ||
        normalized == QStringLiteral("errored")) {
        return TimelineStatus::Failed;
    }
    return TimelineStatus::InProgress;
}

TimelineStatus ToolProjector::resolvedStatus(
    TimelineStatus callStatusValue,
    const QVector<TimelineStatus>& outputStatuses)
{
    if (callStatusValue == TimelineStatus::Failed ||
        outputStatuses.contains(TimelineStatus::Failed)) {
        return TimelineStatus::Failed;
    }
    return outputStatuses.isEmpty()
        ? callStatusValue
        : TimelineStatus::Completed;
}

} // namespace companion
