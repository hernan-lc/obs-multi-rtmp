#pragma once

// PKCE (RFC 7636) generation for the Streamlabs login flow.
//
// - Verifier: 64 secure random bytes, lowercase hex (128 chars).
// - Challenge: base64url(sha256(verifier)) without padding.
//
// Both values are single-use secrets for one login attempt: never log,
// never persist, never reuse. A fresh pair is created per attempt.

#include <QByteArray>

struct PkcePair {
    QByteArray verifier;
    QByteArray challenge;
};

// Generates a fresh random pair. Returns an empty pair only when no
// secure randomness is available (login must then be refused).
PkcePair GeneratePkce();

// Deterministic challenge for a known verifier (used by GeneratePkce
// and by unit tests).
QByteArray PkceChallengeFor(const QByteArray &verifier);
