#include "ui/PairingQrCode.h"

#include "qrcodegen.hpp"

#include <QBuffer>
#include <QImage>
#include <QPainter>
#include <algorithm>
#include <exception>
#include <utility>

namespace {

constexpr int kQuietZoneModules = 4;
constexpr int kDisplayImageSize = 212;

companion::CompanionError qrError(
    QString code,
    QString message)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {},
    };
}

} // namespace

namespace companion {

Result<QString> pairingQrCodeDataUrl(
    const QString& pairingLink)
{
    if (pairingLink.isEmpty()) {
        return Result<QString>::failure(
            qrError(
                QStringLiteral(
                    "pairing.qr-empty-link"),
                QStringLiteral(
                    "The pairing link is empty.")));
    }

    try {
        const QByteArray encoded =
            pairingLink.toUtf8();
        const qrcodegen::QrCode qr =
            qrcodegen::QrCode::encodeText(
                encoded.constData(),
                qrcodegen::QrCode::Ecc::MEDIUM);
        const int modules = qr.getSize();
        const int modulesWithQuietZone =
            modules + (kQuietZoneModules * 2);
        const int scale = std::max(
            1,
            kDisplayImageSize
                / modulesWithQuietZone);
        const int rasterSize =
            modulesWithQuietZone * scale;
        const int imageSize = std::max(
            kDisplayImageSize,
            rasterSize);
        const int rasterOffset =
            (imageSize - rasterSize) / 2;
        QImage image(
            imageSize,
            imageSize,
            QImage::Format_RGB32);
        image.fill(Qt::white);

        QPainter painter(&image);
        painter.setRenderHint(
            QPainter::Antialiasing,
            false);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        for (int y = 0; y < modules; ++y) {
            for (int x = 0; x < modules; ++x) {
                if (!qr.getModule(x, y)) {
                    continue;
                }
                painter.drawRect(
                    rasterOffset
                        + (x + kQuietZoneModules)
                        * scale,
                    rasterOffset
                        + (y + kQuietZoneModules)
                        * scale,
                    scale,
                    scale);
            }
        }
        painter.end();

        QByteArray png;
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly)
            || !image.save(&buffer, "PNG")) {
            return Result<QString>::failure(
                qrError(
                    QStringLiteral(
                        "pairing.qr-png-failed"),
                    QStringLiteral(
                        "The pairing QR image could not be encoded.")));
        }

        return Result<QString>::success(
            QStringLiteral(
                "data:image/png;base64,")
                + QString::fromLatin1(
                    png.toBase64()));
    } catch (const std::exception&) {
        return Result<QString>::failure(
            qrError(
                QStringLiteral(
                    "pairing.qr-generation-failed"),
                QStringLiteral(
                    "The pairing link is too large to encode as a QR code.")));
    }
}

} // namespace companion
