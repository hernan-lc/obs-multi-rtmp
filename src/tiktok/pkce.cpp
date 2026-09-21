#include "tiktok/pkce.h"

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
    QByteArray randomBytes(64, Qt::Uninitialized);
    // QRandomGenerator::system() is a CSPRNG seeded from OS entropy.
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(randomBytes.data()),
                                          static_cast<qsizetype>(randomBytes.size() / 4));
    pair.verifier = randomBytes.toHex().toLower();
    if (pair.verifier.size() != 128)
        return {};
    pair.challenge = PkceChallengeFor(pair.verifier);
    if (pair.challenge.isEmpty())
        return {};
    return pair;
}
