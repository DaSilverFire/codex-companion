#include <array>
#include <cstdio>

#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QSaveFile>
#include <QtEndian>

namespace {

constexpr int kRequiredArgumentCount = 3;
constexpr int kArgumentIndexSource = 1;
constexpr int kArgumentIndexOutput = 2;
constexpr int kIconType = 1;
constexpr quint8 kIconDimension256Sentinel = 0;
constexpr std::array<int, 8> kIconSizes = {16, 20, 24, 32, 40, 48, 64, 256};

#pragma pack(push, 1)
struct IconDirectoryHeader {
    quint16 reserved = 0;
    quint16 type = 1;
    quint16 count = 0;
};

struct IconDirectoryEntry {
    quint8 width = 0;
    quint8 height = 0;
    quint8 colorCount = 0;
    quint8 reserved = 0;
    quint16 planes = 1;
    quint16 bitCount = 32;
    quint32 bytesInResource = 0;
    quint32 imageOffset = 0;
};
#pragma pack(pop)

template <typename T>
void appendStruct(QByteArray& bytes, const T& value)
{
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(T));
}

QImage renderIconFrame(const QImage& source, const int size)
{
    const QImage scaled = source.scaled(size,
                                        size,
                                        Qt::KeepAspectRatio,
                                        Qt::SmoothTransformation);

    QImage canvas(size, size, QImage::Format_ARGB32);
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    const QPoint origin((size - scaled.width()) / 2, (size - scaled.height()) / 2);
    painter.drawImage(origin, scaled);
    return canvas;
}

QByteArray encodePng(const QImage& image)
{
    QByteArray payload;
    QBuffer buffer(&payload);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return {};
    }

    if (!image.save(&buffer, "PNG")) {
        return {};
    }

    return payload;
}

}  // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() != kRequiredArgumentCount) {
        fprintf(stderr, "Usage: companion_icon_compiler <source-png> <output-ico>\n");
        return 1;
    }

    const QString sourcePath = arguments.at(kArgumentIndexSource);
    const QString outputPath = arguments.at(kArgumentIndexOutput);

    QImageReader reader(sourcePath);
    reader.setAutoTransform(true);
    const QImage sourceImage = reader.read();
    if (sourceImage.isNull()) {
        fprintf(stderr, "Failed to load source PNG: %s\n",
                qPrintable(reader.errorString()));
        return 1;
    }

    std::array<QByteArray, kIconSizes.size()> payloads;
    quint32 imageOffset = static_cast<quint32>(
        sizeof(IconDirectoryHeader) + (sizeof(IconDirectoryEntry) * kIconSizes.size()));

    QByteArray iconBytes;
    iconBytes.reserve(static_cast<int>(imageOffset));

    IconDirectoryHeader header;
    header.reserved = qToLittleEndian<quint16>(0);
    header.type = qToLittleEndian<quint16>(kIconType);
    header.count = qToLittleEndian<quint16>(static_cast<quint16>(kIconSizes.size()));
    appendStruct(iconBytes, header);

    std::array<IconDirectoryEntry, kIconSizes.size()> entries;
    for (qsizetype index = 0; index < static_cast<qsizetype>(kIconSizes.size()); ++index) {
        const int size = kIconSizes.at(index);
        const QByteArray payload = encodePng(renderIconFrame(sourceImage, size));
        if (payload.isEmpty()) {
            fprintf(stderr, "Failed to encode PNG payload for size %d\n", size);
            return 1;
        }

        payloads.at(index) = payload;

        IconDirectoryEntry entry;
        entry.width = size == 256 ? kIconDimension256Sentinel : static_cast<quint8>(size);
        entry.height = size == 256 ? kIconDimension256Sentinel : static_cast<quint8>(size);
        entry.colorCount = 0;
        entry.reserved = 0;
        entry.planes = qToLittleEndian<quint16>(1);
        entry.bitCount = qToLittleEndian<quint16>(32);
        entry.bytesInResource =
            qToLittleEndian<quint32>(static_cast<quint32>(payload.size()));
        entry.imageOffset = qToLittleEndian<quint32>(imageOffset);
        entries.at(index) = entry;

        imageOffset += static_cast<quint32>(payload.size());
    }

    for (const IconDirectoryEntry& entry : entries) {
        appendStruct(iconBytes, entry);
    }
    for (const QByteArray& payload : payloads) {
        iconBytes.append(payload);
    }

    QSaveFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly)) {
        fprintf(stderr, "Failed to open output ICO: %s\n",
                qPrintable(output.errorString()));
        return 1;
    }

    if (output.write(iconBytes) != iconBytes.size()) {
        fprintf(stderr, "Failed to write output ICO: %s\n",
                qPrintable(output.errorString()));
        return 1;
    }

    if (!output.commit()) {
        fprintf(stderr, "Failed to commit output ICO: %s\n",
                qPrintable(output.errorString()));
        return 1;
    }

    return 0;
}
