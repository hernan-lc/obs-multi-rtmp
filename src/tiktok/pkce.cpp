#include "tiktok/pkce.h"

#include <array>

#include <QCryptographicHash>
#include <QRandomGenerator>

QByteArray PkceChallengeFor(const QByteArray &verifier)
{
    const QByteArray digest = QCryptographicHash::hash(verifier, QCryptographicHash::Sha256);
    return digest.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

PkcePair GeneratePkce()
{
    PkcePair pair;
    // QRandomGenerator::system() is a CSPRNG seeded from OS entropy.
    // Fill naturally aligned words first: QByteArray storage has no
    // guaranteed quint32 alignment.
    std::array<quint32, 16> randomWords{};
    QRandomGenerator::system()->fillRange(randomWords.data(), randomWords.size());
    const QByteArray randomBytes(reinterpret_cast<const char *>(randomWords.data()),
                                 static_cast<qsizetype>(sizeof(randomWords)));
    pair.verifier = randomBytes.toHex().toLower();
    if (pair.verifier.size() != 128)
        return {};
    pair.challenge = PkceChallengeFor(pair.verifier);
    if (pair.challenge.isEmpty())
        return {};
    return pair;
}
