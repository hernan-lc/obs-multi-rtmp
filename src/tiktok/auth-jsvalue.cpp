#include "tiktok/auth-jsvalue.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

namespace TikTokAuthJsValue {

QVariant JsonToVariant(const QString &json)
{
    const QByteArray utf8 = json.trimmed().toUtf8();
    if (utf8.isEmpty())
        return QVariant();

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(utf8, &error);
    if (error.error == QJsonParseError::NoError) {
        if (document.isNull())
            return QVariant(); // JSON null
        return document.toVariant();
    }

    // Some JSON documents only accept objects and arrays at the top level.
    // Retry with the value wrapped in an array so bare scalars (numbers,
    // booleans and strings) convert exactly like the WebEngine backend.
    const QJsonDocument wrapped = QJsonDocument::fromJson("[" + utf8 + "]", &error);
    if (error.error == QJsonParseError::NoError && wrapped.isArray() && !wrapped.array().isEmpty()) {
        const QJsonValue value = wrapped.array().at(0);
        if (value.isNull())
            return QVariant();
        return value.toVariant();
    }

    // Malformed result: report "no value" instead of a half decoded object.
    return QVariant();
}

} // namespace TikTokAuthJsValue
