#pragma once

// Conversion of JavaScript results into QVariant.
//
// The Qt WebEngine backend returns native QVariant values, while WebView2
// returns the JSON serialization of the JavaScript result. Both paths must
// produce the same shape, otherwise the token exchange would behave
// differently depending on the backend.

#include <QString>
#include <QVariant>

namespace TikTokAuthJsValue {

// Converts a JSON encoded JavaScript result into a QVariant.
//
//   null            -> invalid QVariant
//   true / false    -> bool
//   42              -> double
//   "text"          -> QString
//   [1, 2]          -> QVariantList
//   {"a": 1}        -> QVariantMap
//
// Malformed or empty input yields an invalid QVariant instead of throwing.
QVariant JsonToVariant(const QString &json);

} // namespace TikTokAuthJsValue
