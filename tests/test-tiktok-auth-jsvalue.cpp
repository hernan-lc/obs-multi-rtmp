// JavaScript result conversion tests (WebView2 JSON -> QVariant).
#include <iostream>

#include <QVariantMap>
#include <QVariantList>

#include "tiktok/auth-jsvalue.h"

static int g_checks = 0;
#define CHECK(cond)                                                  \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";      \
            return 1;                                                \
        }                                                            \
    } while (0)

static int TestPrimitives()
{
    CHECK(TikTokAuthJsValue::JsonToVariant(QStringLiteral("\"started\"")) ==
          QVariant(QStringLiteral("started")));
    CHECK(TikTokAuthJsValue::JsonToVariant(QStringLiteral("true")) == QVariant(true));
    CHECK(TikTokAuthJsValue::JsonToVariant(QStringLiteral("false")) == QVariant(false));
    const QVariant number = TikTokAuthJsValue::JsonToVariant(QStringLiteral("42"));
    CHECK(number.isValid());
    CHECK(number.toDouble() == 42.0);
    return 0;
}

static int TestNullAndEmpty()
{
    // Null means "no value yet" for the exchange poll.
    CHECK(!TikTokAuthJsValue::JsonToVariant(QStringLiteral("null")).isValid());
    CHECK(!TikTokAuthJsValue::JsonToVariant(QString()).isValid());
    CHECK(!TikTokAuthJsValue::JsonToVariant(QStringLiteral("   ")).isValid());
    // Malformed input never yields a half decoded value.
    CHECK(!TikTokAuthJsValue::JsonToVariant(QStringLiteral("<html>")).isValid());
    CHECK(!TikTokAuthJsValue::JsonToVariant(QStringLiteral("{broken")).isValid());
    return 0;
}

static int TestObjects()
{
    const QVariant value = TikTokAuthJsValue::JsonToVariant(
        QStringLiteral("{\"success\":true,\"page\":\"streamlabs.com/dashboard\",\"status\":200}"));
    CHECK(value.isValid());
    CHECK(value.typeId() == QMetaType::QVariantMap);
    const QVariantMap map = value.toMap();
    CHECK(map.value(QStringLiteral("success")).toBool());
    CHECK(map.value(QStringLiteral("page")).toString() == QStringLiteral("streamlabs.com/dashboard"));
    CHECK(map.value(QStringLiteral("status")).toInt() == 200);
    return 0;
}

static int TestNestedObjects()
{
    const QVariant value = TikTokAuthJsValue::JsonToVariant(
        QStringLiteral("{\"success\":true,\"data\":{\"success\":true,\"data\":"
                       "{\"oauth_token\":\"FAKE\"}}}"));
    CHECK(value.isValid());
    const QVariantMap map = value.toMap();
    const QVariantMap outer = map.value(QStringLiteral("data")).toMap();
    CHECK(outer.value(QStringLiteral("success")).toBool());
    const QVariantMap inner = outer.value(QStringLiteral("data")).toMap();
    CHECK(inner.value(QStringLiteral("oauth_token")).toString() == QStringLiteral("FAKE"));
    return 0;
}

static int TestArrays()
{
    const QVariant value = TikTokAuthJsValue::JsonToVariant(QStringLiteral("[1,\"two\",false]"));
    CHECK(value.isValid());
    CHECK(value.typeId() == QMetaType::QVariantList);
    const QVariantList list = value.toList();
    CHECK(list.size() == 3);
    CHECK(list.at(0).toInt() == 1);
    CHECK(list.at(1).toString() == QStringLiteral("two"));
    CHECK(list.at(2).toBool() == false);
    return 0;
}

static int TestSurroundingWhitespace()
{
    CHECK(TikTokAuthJsValue::JsonToVariant(QStringLiteral("  \"started\"  ")) ==
          QVariant(QStringLiteral("started")));
    return 0;
}

int main()
{
    if (TestPrimitives())
        return 1;
    if (TestNullAndEmpty())
        return 1;
    if (TestObjects())
        return 1;
    if (TestNestedObjects())
        return 1;
    if (TestArrays())
        return 1;
    if (TestSurroundingWhitespace())
        return 1;
    std::cout << "all " << g_checks << " checks passed\n";
    return 0;
}
