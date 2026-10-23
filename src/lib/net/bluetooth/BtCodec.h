/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace deskflow::net::bt {

//! Framing, credit based flow control and handshake for the Bluetooth transport.
/*!
GATT is message oriented: one write/notification carries a whole attribute
value, and the peer's stack can drop values when its buffers are full.  A KVM
stream therefore needs three things that TCP gives us for free:

1. framing, so stream bytes survive the message boundaries;
2. back pressure, so a fast sender cannot overrun a slow BLE link;
3. a handshake, so both sides agree on the maximum chunk size.

Chunk wire format (little endian):

\verbatim
  +--------+--------+--------+-------------------+
  | type   | len lo | len hi | payload (len)     |
  +--------+--------+--------+-------------------+
\endverbatim

Types:
- \c Data   (0x01) stream bytes, \c len in 1..chunkSize
- \c Credit (0x02) \c len == 2, payload is a uint16 credit grant
- \c Hello  (0x03) \c len == 3, payload is uint8 version + uint16 chunk size
- \c Bye    (0x04) \c len == 0, orderly shutdown

Flow control is a credit window: the sender may transmit at most \c credits
Data chunks.  Every received Data chunk grants one credit back; grants are
aggregated so a bulk transfer does not spend half its bandwidth on credit
chunks (see \c kCreditBatch).

The codec is not thread safe; it is owned and driven by a single thread
(\c BtRelay).
*/
class BtCodec
{
public:
  //! Wire protocol version carried by \c Hello.
  static constexpr quint8 kProtocolVersion = 1;

  //! Bytes of chunk header (type + length).
  static constexpr int kHeaderSize = 3;

  //! Smallest chunk size we accept from a peer.
  static constexpr int kMinChunkSize = 32;

  //! Largest chunk size we accept from a peer (BLE 5.0 extended MTU headroom).
  static constexpr int kMaxChunkSize = 1024;

  //! Number of received Data chunks that trigger one aggregated credit grant.
  static constexpr int kCreditBatch = 8;

  //! Largest amount of unacknowledged stream data we buffer before failing.
  static constexpr int kMaxPendingBytes = 4 * 1024 * 1024;

  explicit BtCodec(int chunkSize = 180, int initialCredits = 32);

  //! @name configuration
  //@{
  void setChunkSize(int chunkSize);
  int chunkSize() const;
  int initialCredits() const;
  //@}

  //! @name sending
  //@{

  //! Queue stream bytes.  Never blocks; the bytes are kept until credit allows.
  void queueStream(const QByteArray &bytes);

  //! Return chunks that may be sent now, in order, honouring the credit window.
  /*!
  Chunks returned are already removed from the codec.  Call this after
  \c queueStream, after \c decode (credits may have been granted) and whenever
  the link reports \c writable.
  */
  QList<QByteArray> takeChunks();

  //! Number of stream bytes waiting for credit.
  int pendingBytes() const;

  //! Credits currently available for Data chunks.
  int credits() const;
  //@}

  //! @name receiving
  //@{

  //! Feed one received chunk; returns the stream bytes it carried.
  /*!
  Credit grants and \c Hello chunks are consumed internally.  On a protocol
  error \c hasError becomes true and an empty array is returned.
  */
  QByteArray decode(const QByteArray &chunk);

  //! Chunk announcing our protocol version and chunk size (send once on connect).
  QByteArray helloChunk() const;

  //! Chunk announcing an orderly shutdown (send before closing the link).
  QByteArray byeChunk() const;
  //@}

  //! @name state
  //@{
  bool hasError() const;
  QString errorString() const;

  //! Chunk size agreed with the peer (before \c Hello it is our own).
  int effectiveChunkSize() const;

  //! True once the peer sent an orderly shutdown (\c Bye).
  bool peerClosed() const;

  //! Reset for a new connection; keeps the configured chunk size.
  void reset();
  //@}

private:
  void fail(const QString &reason);
  void grant(int count);
  QByteArray makeChunk(quint8 type, const QByteArray &payload) const;

  int m_chunkSize = 180;
  int m_initialCredits = 32;
  int m_credits = 32;
  int m_pendingGrant = 0;
  int m_peerChunkSize = 0;
  QByteArray m_out;

  //! Control chunks (credit grants) produced by \c decode, handed out by \c takeChunks.
  QList<QByteArray> m_control;

  bool m_error = false;
  bool m_peerClosed = false;
  QString m_errorString;
};

} // namespace deskflow::net::bt
