/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/bluetooth/BtCodec.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

#include <algorithm>

namespace deskflow::net::bt {
namespace {

//! Chunk type identifiers on the wire (see \c BtCodec).
constexpr quint8 kTypeData = 0x01;
constexpr quint8 kTypeCredit = 0x02;
constexpr quint8 kTypeHello = 0x03;
constexpr quint8 kTypeBye = 0x04;

//! Largest value a uint16 credit counter/grant can hold.
constexpr int kMaxCredit = 0xFFFF;

//! Clamp a requested chunk size into the range the protocol allows.
int clampChunkSize(int size)
{
  return std::clamp(size, BtCodec::kMinChunkSize, BtCodec::kMaxChunkSize);
}

//! Read a little endian uint16 from \p bytes at \p offset.
quint16 readLe16(const QByteArray &bytes, int offset)
{
  const auto lo = static_cast<quint16>(static_cast<quint8>(bytes.at(offset)));
  const auto hi = static_cast<quint16>(static_cast<quint8>(bytes.at(offset + 1)));
  return static_cast<quint16>(lo | static_cast<quint16>(hi << 8));
}

//! Encode \p value as a two byte little endian payload.
QByteArray le16Payload(int value)
{
  const auto clamped = static_cast<quint16>(std::clamp(value, 0, kMaxCredit));
  QByteArray payload(2, '\0');
  payload[0] = static_cast<char>(clamped & 0xFF);
  payload[1] = static_cast<char>((clamped >> 8) & 0xFF);
  return payload;
}

} // namespace

BtCodec::BtCodec(int chunkSize, int initialCredits)
    : m_chunkSize(clampChunkSize(chunkSize)),
      m_initialCredits(std::max(1, initialCredits)),
      m_credits(m_initialCredits)
{
}

void BtCodec::setChunkSize(int chunkSize)
{
  m_chunkSize = clampChunkSize(chunkSize);
}

int BtCodec::chunkSize() const
{
  return m_chunkSize;
}

int BtCodec::initialCredits() const
{
  return m_initialCredits;
}

void BtCodec::queueStream(const QByteArray &bytes)
{
  if (hasError()) {
    return;
  }

  m_out.append(bytes);

  if (m_out.size() > kMaxPendingBytes) {
    fail(QStringLiteral("outgoing buffer overflow: %1 bytes pending").arg(m_out.size()));
  }
}

QList<QByteArray> BtCodec::takeChunks()
{
  QList<QByteArray> chunks;

  if (hasError()) {
    return chunks;
  }

  // Control chunks (credit grants) queued by decode() always go first; they are
  // not flow controlled themselves, otherwise credits could deadlock.
  chunks = m_control;
  m_control.clear();

  while (m_credits > 0 && !m_out.isEmpty()) {
    const int size = std::min(static_cast<int>(m_out.size()), effectiveChunkSize());
    chunks.append(makeChunk(kTypeData, m_out.left(size)));
    m_out.remove(0, size);
    --m_credits;
  }

  // Flush a partial grant so a peer that is down to its last credits is never
  // left waiting for a batch that can no longer be completed.
  if (m_pendingGrant > 0) {
    chunks.append(makeChunk(kTypeCredit, le16Payload(m_pendingGrant)));
    m_pendingGrant = 0;
  }

  return chunks;
}

int BtCodec::pendingBytes() const
{
  return static_cast<int>(m_out.size());
}

int BtCodec::credits() const
{
  return m_credits;
}

QByteArray BtCodec::decode(const QByteArray &chunk)
{
  if (hasError()) {
    return {};
  }

  if (chunk.size() < kHeaderSize) {
    fail(QStringLiteral("truncated chunk header"));
    return {};
  }

  const auto type = static_cast<quint8>(chunk.at(0));
  const int len = readLe16(chunk, 1);

  if (chunk.size() != kHeaderSize + len) {
    fail(QStringLiteral("chunk length mismatch: header says %1, got %2").arg(len).arg(chunk.size() - kHeaderSize));
    return {};
  }

  const QByteArray payload = chunk.mid(kHeaderSize);

  switch (type) {
  case kTypeData:
    if (len == 0) {
      fail(QStringLiteral("empty data chunk"));
      return {};
    }
    if (len > kMaxChunkSize) {
      fail(QStringLiteral("data chunk too large: %1").arg(len));
      return {};
    }
    // Every data chunk frees one credit on the sender's side; aggregate the
    // grants so a bulk transfer does not spend its bandwidth on credit chunks.
    ++m_pendingGrant;
    if (m_pendingGrant >= kCreditBatch) {
      m_control.append(makeChunk(kTypeCredit, le16Payload(m_pendingGrant)));
      m_pendingGrant = 0;
    }
    return payload;

  case kTypeCredit:
    if (len != 2) {
      fail(QStringLiteral("credit chunk length mismatch: header says %1, expected 2").arg(len));
      return {};
    }
    grant(readLe16(payload, 0));
    return {};

  case kTypeHello:
    if (len != 3) {
      fail(QStringLiteral("hello chunk length mismatch: header says %1, expected 3").arg(len));
      return {};
    }
    if (static_cast<quint8>(payload.at(0)) != kProtocolVersion) {
      fail(QStringLiteral("unsupported peer protocol version %1").arg(static_cast<quint8>(payload.at(0))));
      return {};
    }
    m_peerChunkSize = clampChunkSize(readLe16(payload, 1));
    return {};

  case kTypeBye:
    if (len != 0) {
      fail(QStringLiteral("bye chunk length mismatch: header says %1, expected 0").arg(len));
      return {};
    }
    m_peerClosed = true;
    return {};

  default:
    fail(QStringLiteral("unknown chunk type 0x%1").arg(type, 2, 16, QChar('0')));
    return {};
  }
}

QByteArray BtCodec::helloChunk() const
{
  QByteArray payload;
  payload.append(static_cast<char>(kProtocolVersion));
  payload.append(le16Payload(m_chunkSize));
  return makeChunk(kTypeHello, payload);
}

QByteArray BtCodec::byeChunk() const
{
  return makeChunk(kTypeBye, QByteArray());
}

bool BtCodec::hasError() const
{
  return m_error;
}

QString BtCodec::errorString() const
{
  return m_errorString;
}

int BtCodec::effectiveChunkSize() const
{
  if (m_peerChunkSize > 0) {
    return std::min(m_chunkSize, m_peerChunkSize);
  }
  return m_chunkSize;
}

bool BtCodec::peerClosed() const
{
  return m_peerClosed;
}

void BtCodec::reset()
{
  m_out.clear();
  m_control.clear();
  m_credits = m_initialCredits;
  m_pendingGrant = 0;
  m_peerChunkSize = 0;
  m_error = false;
  m_errorString.clear();
  m_peerClosed = false;
}

void BtCodec::fail(const QString &reason)
{
  m_error = true;
  m_errorString = reason;
}

void BtCodec::grant(int count)
{
  m_credits = std::min(m_credits + count, kMaxCredit);
}

QByteArray BtCodec::makeChunk(quint8 type, const QByteArray &payload) const
{
  QByteArray chunk;
  chunk.reserve(kHeaderSize + payload.size());
  chunk.append(static_cast<char>(type));
  chunk.append(static_cast<char>(payload.size() & 0xFF));
  chunk.append(static_cast<char>((payload.size() >> 8) & 0xFF));
  chunk.append(payload);
  return chunk;
}

} // namespace deskflow::net::bt
