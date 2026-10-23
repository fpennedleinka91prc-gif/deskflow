/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/bluetooth/BtCodec.h"

#include <QByteArray>
#include <QTest>

using deskflow::net::bt::BtCodec;

namespace {

constexpr quint8 kTypeData = 0x01;
constexpr quint8 kTypeCredit = 0x02;
constexpr quint8 kTypeHello = 0x03;
constexpr quint8 kTypeBye = 0x04;

//! Build a chunk exactly the way the codec does, so malformed input can be crafted.
QByteArray makeChunk(quint8 type, const QByteArray &payload)
{
  QByteArray chunk;
  chunk.append(static_cast<char>(type));
  chunk.append(static_cast<char>(payload.size() & 0xFF));
  chunk.append(static_cast<char>((payload.size() >> 8) & 0xFF));
  chunk.append(payload);
  return chunk;
}

quint8 chunkType(const QByteArray &chunk)
{
  return static_cast<quint8>(chunk.at(0));
}

int chunkPayloadSize(const QByteArray &chunk)
{
  return static_cast<int>(chunk.size()) - BtCodec::kHeaderSize;
}

quint16 chunkCreditValue(const QByteArray &chunk)
{
  const auto lo = static_cast<quint16>(static_cast<quint8>(chunk.at(3)));
  const auto hi = static_cast<quint16>(static_cast<quint8>(chunk.at(4)));
  return static_cast<quint16>(lo | static_cast<quint16>(hi << 8));
}

//! Deterministic, non repeating test payload.
QByteArray makePayload(int size)
{
  QByteArray payload(size, '\0');
  for (int i = 0; i < size; ++i) {
    payload[i] = static_cast<char>((i * 31 + 7) & 0xFF);
  }
  return payload;
}

} // namespace

class BluetoothCodecTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void test_construction();
  void test_roundTrip();
  void test_creditWindow();
  void test_creditBatching();
  void test_chunkSizeNegotiation();
  void test_overflow();
  void test_malformedChunks();
  void test_bye();
  void test_reset();
};

void BluetoothCodecTests::test_construction()
{
  BtCodec defaults;
  QCOMPARE(defaults.chunkSize(), 180);
  QCOMPARE(defaults.initialCredits(), 32);
  QCOMPARE(defaults.credits(), 32);
  QCOMPARE(defaults.effectiveChunkSize(), 180);
  QCOMPARE(defaults.pendingBytes(), 0);
  QVERIFY(!defaults.hasError());
  QVERIFY(defaults.errorString().isEmpty());
  QVERIFY(!defaults.peerClosed());
  QVERIFY(defaults.takeChunks().isEmpty());

  // Chunk size is clamped into [kMinChunkSize, kMaxChunkSize].
  BtCodec tiny(1, 4);
  QCOMPARE(tiny.chunkSize(), BtCodec::kMinChunkSize);

  BtCodec huge(99999, 4);
  QCOMPARE(huge.chunkSize(), BtCodec::kMaxChunkSize);

  // At least one credit is always available, otherwise the codec could never send.
  BtCodec noCredits(180, 0);
  QCOMPARE(noCredits.initialCredits(), 1);
  QCOMPARE(noCredits.credits(), 1);

  BtCodec negativeCredits(180, -5);
  QCOMPARE(negativeCredits.initialCredits(), 1);
  QCOMPARE(negativeCredits.credits(), 1);

  BtCodec resizable;
  resizable.setChunkSize(1);
  QCOMPARE(resizable.chunkSize(), BtCodec::kMinChunkSize);
  resizable.setChunkSize(500);
  QCOMPARE(resizable.chunkSize(), 500);
  resizable.setChunkSize(100000);
  QCOMPARE(resizable.chunkSize(), BtCodec::kMaxChunkSize);
}

void BluetoothCodecTests::test_roundTrip()
{
  // Small chunk size and a narrow credit window force several pump rounds.
  BtCodec sender(64, 4);
  BtCodec receiver;

  const QByteArray payload = makePayload(5000);
  sender.queueStream(payload);
  QCOMPARE(sender.pendingBytes(), static_cast<int>(payload.size()));

  QByteArray reassembled;
  for (int guard = 0; guard < 10000 && sender.pendingBytes() > 0; ++guard) {
    const auto chunks = sender.takeChunks();
    QVERIFY(!chunks.isEmpty());

    for (const auto &chunk : chunks) {
      if (chunkType(chunk) != kTypeData) {
        continue;
      }
      // Data chunks never exceed the agreed chunk size.
      QVERIFY(chunkPayloadSize(chunk) <= sender.effectiveChunkSize());
      QVERIFY(chunkPayloadSize(chunk) <= BtCodec::kMaxChunkSize);
      reassembled.append(receiver.decode(chunk));
    }

    QVERIFY(!sender.hasError());
    QVERIFY(!receiver.hasError());

    // Recycle the credit grants so the sender can keep going.
    for (const auto &chunk : receiver.takeChunks()) {
      QCOMPARE(chunkType(chunk), kTypeCredit);
      sender.decode(chunk);
    }
  }

  QCOMPARE(sender.pendingBytes(), 0);
  QCOMPARE(reassembled.size(), payload.size());
  QCOMPARE(reassembled, payload);
  QVERIFY(!sender.hasError());
  QVERIFY(!receiver.hasError());

  // Everything is drained: no data left, and the receiver granted every credit back.
  QVERIFY(sender.takeChunks().isEmpty());
  QCOMPARE(sender.credits(), sender.initialCredits());
  QVERIFY(receiver.takeChunks().isEmpty());
}

void BluetoothCodecTests::test_creditWindow()
{
  BtCodec sender(64, 2);
  BtCodec receiver;

  sender.queueStream(makePayload(1000));

  auto chunks = sender.takeChunks();
  QCOMPARE(static_cast<int>(chunks.size()), 2);
  QCOMPARE(chunkType(chunks.at(0)), kTypeData);
  QCOMPARE(chunkType(chunks.at(1)), kTypeData);
  QCOMPARE(chunkPayloadSize(chunks.at(0)), 64);
  QCOMPARE(chunkPayloadSize(chunks.at(1)), 64);
  QCOMPARE(sender.credits(), 0);
  QCOMPARE(sender.pendingBytes(), 1000 - 128);

  // No credits left: nothing more may be emitted.
  QVERIFY(sender.takeChunks().isEmpty());
  QCOMPARE(sender.pendingBytes(), 1000 - 128);

  // The receiver returns what it consumed (a partial grant is flushed at once).
  for (const auto &chunk : chunks) {
    receiver.decode(chunk);
  }
  const auto grants = receiver.takeChunks();
  QCOMPARE(static_cast<int>(grants.size()), 1);
  QCOMPARE(chunkType(grants.at(0)), kTypeCredit);
  QCOMPARE(chunkCreditValue(grants.at(0)), static_cast<quint16>(2));

  sender.decode(grants.at(0));
  QCOMPARE(sender.credits(), 2);
  QCOMPARE(sender.takeChunks().size(), 2);
  QCOMPARE(sender.credits(), 0);
}

void BluetoothCodecTests::test_creditBatching()
{
  BtCodec sender(64, 1000);
  BtCodec receiver;

  // Exactly one batch worth of data chunks.
  sender.queueStream(makePayload(BtCodec::kCreditBatch * 64));
  const auto dataChunks = sender.takeChunks();
  QCOMPARE(static_cast<int>(dataChunks.size()), BtCodec::kCreditBatch);

  for (const auto &chunk : dataChunks) {
    QCOMPARE(chunkType(chunk), kTypeData);
    QVERIFY(receiver.decode(chunk).size() > 0);
  }

  const auto grants = receiver.takeChunks();
  QCOMPARE(static_cast<int>(grants.size()), 1);
  QCOMPARE(chunkType(grants.at(0)), kTypeCredit);
  QCOMPARE(chunkCreditValue(grants.at(0)), static_cast<quint16>(BtCodec::kCreditBatch));

  // A full batch plus a remainder yields the batch immediately and the
  // remainder when the chunks are taken, so no credit is ever stranded.
  BtCodec secondReceiver;
  for (int i = 0; i < BtCodec::kCreditBatch + 1; ++i) {
    secondReceiver.decode(makeChunk(kTypeData, QByteArray(8, 'd')));
  }
  const auto batched = secondReceiver.takeChunks();
  QCOMPARE(static_cast<int>(batched.size()), 2);
  QCOMPARE(chunkType(batched.at(0)), kTypeCredit);
  QCOMPARE(chunkCreditValue(batched.at(0)), static_cast<quint16>(BtCodec::kCreditBatch));
  QCOMPARE(chunkType(batched.at(1)), kTypeCredit);
  QCOMPARE(chunkCreditValue(batched.at(1)), static_cast<quint16>(1));

  // Nothing left to grant.
  QVERIFY(secondReceiver.takeChunks().isEmpty());
}

void BluetoothCodecTests::test_chunkSizeNegotiation()
{
  BtCodec big(1024, 64);
  BtCodec peer(512, 32);

  // Our own Hello carries the version and our own chunk size.
  const auto hello = big.helloChunk();
  QCOMPARE(static_cast<int>(hello.size()), BtCodec::kHeaderSize + 3);
  QCOMPARE(chunkType(hello), kTypeHello);
  QCOMPARE(static_cast<quint8>(hello.at(3)), BtCodec::kProtocolVersion);
  QCOMPARE(chunkPayloadSize(hello), 3);

  // Before the handshake the codec uses its own chunk size.
  QCOMPARE(big.effectiveChunkSize(), 1024);

  QVERIFY(big.decode(peer.helloChunk()).isEmpty());
  QVERIFY(!big.hasError());
  QCOMPARE(big.effectiveChunkSize(), 512);

  big.queueStream(makePayload(3000));
  const auto chunks = big.takeChunks();
  QVERIFY(chunks.size() >= 6);
  for (const auto &chunk : chunks) {
    QCOMPARE(chunkType(chunk), kTypeData);
    QVERIFY(chunkPayloadSize(chunk) <= 512);
  }
  QCOMPARE(chunkPayloadSize(chunks.at(0)), 512);

  // A peer offering more than we configured does not raise our limit.
  BtCodec small(180, 32);
  QVERIFY(small.decode(peer.helloChunk()).isEmpty());
  QCOMPARE(small.effectiveChunkSize(), 180);

  // A peer offering less than the minimum is clamped up to kMinChunkSize.
  BtCodec clamped(180, 32);
  QByteArray tinyHello;
  tinyHello.append(static_cast<char>(BtCodec::kProtocolVersion));
  tinyHello.append(static_cast<char>(8));
  tinyHello.append(static_cast<char>(0));
  QVERIFY(clamped.decode(makeChunk(kTypeHello, tinyHello)).isEmpty());
  QCOMPARE(clamped.effectiveChunkSize(), BtCodec::kMinChunkSize);
}

void BluetoothCodecTests::test_overflow()
{
  BtCodec codec;

  // Exactly at the limit is still accepted.
  codec.queueStream(QByteArray(BtCodec::kMaxPendingBytes, 'x'));
  QCOMPARE(codec.pendingBytes(), BtCodec::kMaxPendingBytes);
  QVERIFY(!codec.hasError());

  // One byte more fails the codec.
  codec.queueStream(QByteArray(1, 'x'));
  QVERIFY(codec.hasError());
  QVERIFY(!codec.errorString().isEmpty());
  QVERIFY(codec.errorString().contains(QStringLiteral("overflow")));

  // A failed codec refuses further work.
  codec.queueStream(QByteArray(16, 'x'));
  QVERIFY(codec.takeChunks().isEmpty());
  QVERIFY(codec.decode(makeChunk(kTypeData, QByteArray(4, 'd'))).isEmpty());
}

void BluetoothCodecTests::test_malformedChunks()
{
  // Truncated header: fewer bytes than the fixed header.
  {
    BtCodec codec;
    QVERIFY(codec.decode(QByteArray("\x01\x05", 2)).isEmpty());
    QVERIFY(codec.hasError());
    QVERIFY(!codec.errorString().isEmpty());
    QVERIFY(codec.errorString().contains(QStringLiteral("truncated")));
  }

  // Length mismatch: header claims more bytes than the chunk carries.
  {
    BtCodec codec;
    QByteArray chunk;
    chunk.append(static_cast<char>(kTypeData));
    chunk.append(static_cast<char>(5));
    chunk.append(static_cast<char>(0));
    chunk.append(QByteArray(2, 'a'));
    QVERIFY(codec.decode(chunk).isEmpty());
    QVERIFY(codec.hasError());
    QVERIFY(codec.errorString().contains(QStringLiteral("mismatch")));
  }

  // Length mismatch the other way round: header claims fewer bytes than carried.
  {
    BtCodec codec;
    QVERIFY(codec.decode(makeChunk(kTypeData, QByteArray(4, 'a')) + QByteArray(1, 'b')).isEmpty());
    QVERIFY(codec.hasError());
    QVERIFY(!codec.errorString().isEmpty());
  }

  // Unknown chunk type.
  {
    BtCodec codec;
    QVERIFY(codec.decode(makeChunk(0x09, QByteArray())).isEmpty());
    QVERIFY(codec.hasError());
    QVERIFY(codec.errorString().contains(QStringLiteral("unknown chunk type")));
    QVERIFY(codec.errorString().contains(QStringLiteral("09")));
  }

  // Empty data chunk.
  {
    BtCodec codec;
    QVERIFY(codec.decode(makeChunk(kTypeData, QByteArray())).isEmpty());
    QVERIFY(codec.hasError());
    QVERIFY(codec.errorString().contains(QStringLiteral("empty data chunk")));
  }

  // Oversized data chunk: valid framing, payload beyond kMaxChunkSize.
  {
    BtCodec codec;
    QVERIFY(codec.decode(makeChunk(kTypeData, QByteArray(BtCodec::kMaxChunkSize + 1, 'a'))).isEmpty());
    QVERIFY(codec.hasError());
    QVERIFY(codec.errorString().contains(QStringLiteral("too large")));
  }

  // Bad Hello: unsupported protocol version.
  {
    BtCodec codec;
    QByteArray payload;
    payload.append(static_cast<char>(BtCodec::kProtocolVersion + 1));
    payload.append(static_cast<char>(180));
    payload.append(static_cast<char>(0));
    QVERIFY(codec.decode(makeChunk(kTypeHello, payload)).isEmpty());
    QVERIFY(codec.hasError());
    QVERIFY(codec.errorString().contains(QStringLiteral("protocol version")));
    // A rejected Hello must not change the negotiated chunk size.
    QCOMPARE(codec.effectiveChunkSize(), codec.chunkSize());
  }

  // Bad Hello length.
  {
    BtCodec codec;
    QVERIFY(codec.decode(makeChunk(kTypeHello, QByteArray(2, '\0'))).isEmpty());
    QVERIFY(codec.hasError());
    QVERIFY(!codec.errorString().isEmpty());
  }

  // Bad Credit length.
  {
    BtCodec codec;
    QVERIFY(codec.decode(makeChunk(kTypeCredit, QByteArray(1, '\0'))).isEmpty());
    QVERIFY(codec.hasError());
    QVERIFY(!codec.errorString().isEmpty());
    QCOMPARE(codec.credits(), codec.initialCredits());
  }

  // Bad Bye length.
  {
    BtCodec codec;
    QVERIFY(codec.decode(makeChunk(kTypeBye, QByteArray(1, '\0'))).isEmpty());
    QVERIFY(codec.hasError());
    QVERIFY(!codec.errorString().isEmpty());
    QVERIFY(!codec.peerClosed());
  }

  // Once failed, every further decode is a no-op.
  {
    BtCodec codec;
    codec.decode(makeChunk(0x7f, QByteArray()));
    QVERIFY(codec.hasError());
    QVERIFY(codec.decode(makeChunk(kTypeData, QByteArray(4, 'a'))).isEmpty());
    QVERIFY(codec.hasError());
  }
}

void BluetoothCodecTests::test_bye()
{
  BtCodec sender;
  BtCodec receiver;

  QVERIFY(!receiver.peerClosed());
  QVERIFY(receiver.decode(sender.byeChunk()).isEmpty());
  QVERIFY(!receiver.hasError());
  QVERIFY(receiver.peerClosed());

  // Bye is header plus empty payload.
  const auto bye = sender.byeChunk();
  QCOMPARE(static_cast<int>(bye.size()), BtCodec::kHeaderSize);
  QCOMPARE(chunkType(bye), kTypeBye);
  QCOMPARE(chunkPayloadSize(bye), 0);
}

void BluetoothCodecTests::test_reset()
{
  BtCodec codec(256, 3);
  BtCodec peer;

  // Drive the codec into an error state.
  codec.queueStream(makePayload(1000));
  QCOMPARE(static_cast<int>(codec.takeChunks().size()), 3);
  QCOMPARE(codec.credits(), 0);
  QCOMPARE(codec.pendingBytes(), 1000 - 3 * 256);
  codec.decode(makeChunk(0x0b, QByteArray()));
  QVERIFY(codec.hasError());

  codec.reset();

  QVERIFY(!codec.hasError());
  QVERIFY(codec.errorString().isEmpty());
  QVERIFY(!codec.peerClosed());
  QCOMPARE(codec.credits(), 3);
  QCOMPARE(codec.pendingBytes(), 0);
  // The configured chunk size survives a reset.
  QCOMPARE(codec.chunkSize(), 256);
  // So does the peer chunk size being forgotten.
  QCOMPARE(codec.effectiveChunkSize(), 256);

  // The codec is usable again in both directions.
  const QByteArray payload = makePayload(700);
  codec.queueStream(payload);
  const auto chunks = codec.takeChunks();
  QCOMPARE(static_cast<int>(chunks.size()), 3);

  QByteArray reassembled;
  for (const auto &chunk : chunks) {
    reassembled.append(peer.decode(chunk));
  }
  QCOMPARE(reassembled, payload.left(3 * 256));

  // A Bye flag is cleared by reset too.
  BtCodec closed;
  closed.decode(closed.byeChunk());
  QVERIFY(closed.peerClosed());
  closed.reset();
  QVERIFY(!closed.peerClosed());
}

QTEST_MAIN(BluetoothCodecTests)

#include "BluetoothCodecTests.moc"
