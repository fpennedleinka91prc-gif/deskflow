/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "net/bluetooth/BtTypes.h"
#include "net/bluetooth/IBtLink.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QQueue>
#include <QString>

class QTcpServer;
class QTcpSocket;
class QTimer;

namespace deskflow::net::bt {

//! Radio free \c IBtLink backend used by the end to end tests.
/*!
This backend speaks plain TCP on the loopback interface but keeps the two
properties that make a Bluetooth link different from a byte stream:

- message orientation: every chunk travels as \c [uint32 LE length][payload],
  so a chunk can never be split or merged by the transport;
- back pressure: the send queue is bounded (chunks *and* bytes).  When it is
  full \c sendChunk returns \c false and \c writable is emitted once the queue
  drains, exactly like a BLE notification queue that ran out of buffers.

It needs nothing but \c Qt6::Core and \c Qt6::Network, so the transport and the
relay can be tested on machines (and CI images) without any Bluetooth radio.

Environment:

- \c DESKFLOW_BT_LOOPBACK_PORT: TCP port.  Server: listen port, \c 0 means
  ephemeral (the real port is reported through \c describe()).  Client: port to
  dial, required.
- \c DESKFLOW_BT_LOOPBACK_CHUNK: chunk size reported by \c maxChunkSize(),
  clamped to the range \c BtCodec accepts (default 180).
*/
class BtLoopbackLink : public IBtLink
{
  Q_OBJECT
public:
  explicit BtLoopbackLink(QObject *parent = nullptr);
  ~BtLoopbackLink() override;

  BtLoopbackLink(const BtLoopbackLink &) = delete;
  BtLoopbackLink &operator=(const BtLoopbackLink &) = delete;

  // IBtLink overrides
  void startServer(const Config &config) override;
  void startClient(const Config &config) override;
  void stop() override;
  bool isConnected() const override;
  int maxChunkSize() const override;
  bool sendChunk(const QByteArray &chunk) override;
  QString describe() const override;

private:
  void startListening();
  void startConnecting();
  void connectAttempt();
  void onNewConnection();
  void attachSocket(QTcpSocket *socket);
  void releaseSocket();
  void onSocketConnected();
  void onReadyRead();
  void onSocketError(QAbstractSocket::SocketError error);
  void onSocketDisconnected();
  void onConnectTimeout();
  void onRetryTimeout();
  void handleLoss(const QString &reason);

  void consumeIncoming();
  void drainQueue();
  void scheduleWritable();

  void markConnected();
  void markDisconnected(const QString &reason);
  void fail(const QString &message);

  static quint16 envPort();
  static int envChunkSize();
  static QByteArray encodeFrame(const QByteArray &chunk);

  Mode m_mode = Mode::Server;
  Config m_config;
  QTcpServer *m_server = nullptr;
  QTcpSocket *m_socket = nullptr;
  QTimer *m_retryTimer = nullptr;
  QTimer *m_connectTimer = nullptr;
  QQueue<QByteArray> m_outQueue;
  QByteArray m_inBuffer;
  int m_outChunks = 0;
  int m_outBytes = 0;
  int m_chunkSize = 180;
  quint16 m_port = 0;
  bool m_connected = false;
  bool m_stopping = false;
  bool m_blocked = false;
  bool m_writableScheduled = false;
};

} // namespace deskflow::net::bt
