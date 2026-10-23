/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/bluetooth/BtLoopbackLink.h"

#include "base/Log.h"
#include "net/bluetooth/BtCodec.h"

#include <QHostAddress>
#include <QMetaObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>
#include <QtGlobal>

#include <cstring>

namespace deskflow::net::bt {

namespace {

//! Bytes of the loopback frame header: a little endian uint32 payload length.
constexpr int kFrameHeaderSize = 4;

//! Chunk size reported when neither the config nor the environment say otherwise.
constexpr int kDefaultChunkSize = 180;

//! Send queue limits.  Reached only when the socket buffer is saturated, which
//! is exactly the situation a real BLE link runs into.
constexpr int kMaxQueuedChunks = 64;
constexpr int kMaxQueuedBytes = 256 * 1024;

//! Queue levels at which \c writable is reported again (hysteresis).
constexpr int kResumeChunks = kMaxQueuedChunks / 2;
constexpr int kResumeBytes = kMaxQueuedBytes / 2;

//! Largest frame we accept from a peer before declaring the stream corrupt.
constexpr int kMaxIncomingFrame = BtCodec::kMaxChunkSize + 64;

constexpr int kDefaultRetryMs = 2000;
constexpr int kDefaultConnectTimeoutMs = 30000;

int clampChunkSize(int size)
{
  return qBound(BtCodec::kMinChunkSize, size, BtCodec::kMaxChunkSize);
}

} // namespace

BtLoopbackLink::BtLoopbackLink(QObject *parent) : IBtLink(parent)
{
  m_retryTimer = new QTimer(this);
  m_retryTimer->setSingleShot(true);
  connect(m_retryTimer, &QTimer::timeout, this, &BtLoopbackLink::onRetryTimeout);

  m_connectTimer = new QTimer(this);
  m_connectTimer->setSingleShot(true);
  connect(m_connectTimer, &QTimer::timeout, this, &BtLoopbackLink::onConnectTimeout);
}

BtLoopbackLink::~BtLoopbackLink()
{
  m_stopping = true;
  if (m_socket != nullptr) {
    m_socket->abort();
  }
  if (m_server != nullptr) {
    m_server->close();
  }
}

void BtLoopbackLink::startServer(const Config &config)
{
  m_mode = Mode::Server;
  m_config = config;
  m_stopping = false;
  m_chunkSize = config.chunkSize > 0 ? clampChunkSize(config.chunkSize) : envChunkSize();
  startListening();
}

void BtLoopbackLink::startClient(const Config &config)
{
  m_mode = Mode::Client;
  m_config = config;
  m_stopping = false;
  m_chunkSize = config.chunkSize > 0 ? clampChunkSize(config.chunkSize) : envChunkSize();
  startConnecting();
}

void BtLoopbackLink::stop()
{
  m_stopping = true;
  m_retryTimer->stop();
  m_connectTimer->stop();
  releaseSocket();

  if (m_server != nullptr) {
    m_server->close();
    m_server->disconnect(this);
    m_server->deleteLater();
    m_server = nullptr;
  }

  if (m_connected) {
    markDisconnected(QStringLiteral("stopped"));
  }
}

bool BtLoopbackLink::isConnected() const
{
  return m_connected;
}

int BtLoopbackLink::maxChunkSize() const
{
  return clampChunkSize(m_chunkSize);
}

bool BtLoopbackLink::sendChunk(const QByteArray &chunk)
{
  if (!m_connected || m_socket == nullptr || m_socket->state() != QAbstractSocket::ConnectedState) {
    return false;
  }

  const QByteArray frame = encodeFrame(chunk);
  const int pendingBytes = m_outBytes + int(m_socket->bytesToWrite());
  if (m_outChunks >= kMaxQueuedChunks || pendingBytes + frame.size() > kMaxQueuedBytes) {
    // The peer (or this process) is not keeping up; the caller keeps the chunk.
    m_blocked = true;
    return false;
  }

  m_outQueue.enqueue(frame);
  ++m_outChunks;
  m_outBytes += frame.size();
  drainQueue();
  return true;
}

QString BtLoopbackLink::describe() const
{
  const QString state = m_connected ? QStringLiteral("connected") : QStringLiteral("waiting");
  if (m_mode == Mode::Server) {
    return QStringLiteral("loopback server :%1 (%2)").arg(m_port).arg(state);
  }
  return QStringLiteral("loopback :%1 (%2)").arg(m_port).arg(state);
}

void BtLoopbackLink::startListening()
{
  m_server = new QTcpServer(this);
  connect(m_server, &QTcpServer::newConnection, this, &BtLoopbackLink::onNewConnection);

  const quint16 requested = envPort();
  if (!m_server->listen(QHostAddress::LocalHost, requested)) {
    fail(QStringLiteral("loopback: cannot listen on 127.0.0.1:%1: %2").arg(requested).arg(m_server->errorString()));
    return;
  }

  m_port = m_server->serverPort();
  LOG_DEBUG("bluetooth: loopback listening on 127.0.0.1:%d (chunk %d)", int(m_port), maxChunkSize());
}

void BtLoopbackLink::startConnecting()
{
  m_port = envPort();
  if (m_port == 0) {
    fail(QStringLiteral("loopback: DESKFLOW_BT_LOOPBACK_PORT must name the peer port (client mode)"));
    return;
  }

  LOG_DEBUG("bluetooth: loopback connecting to 127.0.0.1:%d (chunk %d)", int(m_port), maxChunkSize());
  connectAttempt();
}

void BtLoopbackLink::connectAttempt()
{
  if (m_stopping) {
    return;
  }

  releaseSocket();

  m_socket = new QTcpSocket(this);
  connect(m_socket, &QAbstractSocket::connected, this, &BtLoopbackLink::onSocketConnected);
  connect(m_socket, &QAbstractSocket::readyRead, this, &BtLoopbackLink::onReadyRead);
  connect(m_socket, &QAbstractSocket::disconnected, this, &BtLoopbackLink::onSocketDisconnected);
  connect(m_socket, &QAbstractSocket::errorOccurred, this, &BtLoopbackLink::onSocketError);
  connect(m_socket, &QAbstractSocket::bytesWritten, this, [this](qint64) { drainQueue(); });

  m_connectTimer->start(m_config.connectTimeoutMs > 0 ? m_config.connectTimeoutMs : kDefaultConnectTimeoutMs);
  m_socket->connectToHost(QHostAddress::LocalHost, m_port);
}

void BtLoopbackLink::onNewConnection()
{
  while (m_server != nullptr && m_server->hasPendingConnections()) {
    QTcpSocket *socket = m_server->nextPendingConnection();
    if (socket == nullptr) {
      return;
    }
    if (m_socket != nullptr) {
      // A BLE peripheral talks to one central at a time.
      LOG_WARN("bluetooth: loopback peer refused, a peer is already attached");
      socket->abort();
      socket->deleteLater();
      continue;
    }
    attachSocket(socket);
  }
}

void BtLoopbackLink::attachSocket(QTcpSocket *socket)
{
  m_socket = socket;
  m_inBuffer.clear();
  m_outQueue.clear();
  m_outChunks = 0;
  m_outBytes = 0;
  m_blocked = false;

  m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
  connect(m_socket, &QAbstractSocket::connected, this, &BtLoopbackLink::onSocketConnected);
  connect(m_socket, &QAbstractSocket::readyRead, this, &BtLoopbackLink::onReadyRead);
  connect(m_socket, &QAbstractSocket::disconnected, this, &BtLoopbackLink::onSocketDisconnected);
  connect(m_socket, &QAbstractSocket::errorOccurred, this, &BtLoopbackLink::onSocketError);
  connect(m_socket, &QAbstractSocket::bytesWritten, this, [this](qint64) { drainQueue(); });

  markConnected();
}

void BtLoopbackLink::releaseSocket()
{
  if (m_socket == nullptr) {
    return;
  }

  QTcpSocket *socket = m_socket;
  m_socket = nullptr;
  socket->disconnect(this);
  socket->abort();
  socket->deleteLater();

  m_inBuffer.clear();
  m_outQueue.clear();
  m_outChunks = 0;
  m_outBytes = 0;
  m_blocked = false;
}

void BtLoopbackLink::onSocketConnected()
{
  m_connectTimer->stop();
  markConnected();
}

void BtLoopbackLink::onReadyRead()
{
  if (m_socket == nullptr) {
    return;
  }

  m_inBuffer.append(m_socket->readAll());
  consumeIncoming();
}

void BtLoopbackLink::onSocketError(QAbstractSocket::SocketError error)
{
  Q_UNUSED(error)
  const QString reason = m_socket != nullptr ? m_socket->errorString() : QStringLiteral("socket error");
  handleLoss(reason);
}

void BtLoopbackLink::onSocketDisconnected()
{
  handleLoss(QStringLiteral("peer closed the loopback connection"));
}

void BtLoopbackLink::onConnectTimeout()
{
  handleLoss(QStringLiteral("loopback connect timeout"));
}

void BtLoopbackLink::onRetryTimeout()
{
  if (m_stopping) {
    return;
  }
  if (m_mode == Mode::Client) {
    connectAttempt();
  }
}

void BtLoopbackLink::handleLoss(const QString &reason)
{
  if (m_stopping) {
    return;
  }

  const bool wasConnected = m_connected;
  m_connectTimer->stop();
  releaseSocket();

  if (wasConnected) {
    markDisconnected(reason);
  } else {
    LOG_DEBUG("bluetooth: loopback attempt failed: %s", qPrintable(reason));
  }

  if (m_mode == Mode::Client) {
    const int delay = m_config.retryIntervalMs > 0 ? m_config.retryIntervalMs : kDefaultRetryMs;
    m_retryTimer->start(delay);
  }
  // In server mode the listen socket stays open: the next central is accepted.
}

void BtLoopbackLink::consumeIncoming()
{
  while (m_inBuffer.size() >= kFrameHeaderSize) {
    const quint32 length = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(m_inBuffer.constData()));
    if (length > quint32(kMaxIncomingFrame)) {
      fail(QStringLiteral("loopback: oversized chunk (%1 bytes) from peer").arg(length));
      return;
    }
    if (m_inBuffer.size() < kFrameHeaderSize + int(length)) {
      // Wait for the rest of the frame.
      return;
    }

    const QByteArray chunk = m_inBuffer.mid(kFrameHeaderSize, int(length));
    m_inBuffer.remove(0, kFrameHeaderSize + int(length));
    if (!chunk.isEmpty()) {
      Q_EMIT chunkReceived(chunk);
    }
  }
}

void BtLoopbackLink::drainQueue()
{
  if (m_socket == nullptr || m_socket->state() != QAbstractSocket::ConnectedState) {
    return;
  }

  while (!m_outQueue.isEmpty()) {
    const QByteArray frame = m_outQueue.dequeue();
    --m_outChunks;
    m_outBytes -= frame.size();

    const qint64 written = m_socket->write(frame);
    if (written < 0) {
      LOG_WARN("bluetooth: loopback write failed: %s", qPrintable(m_socket->errorString()));
      m_outQueue.clear();
      m_outChunks = 0;
      m_outBytes = 0;
      handleLoss(m_socket->errorString());
      return;
    }
    if (written < frame.size()) {
      // The socket buffer is full; keep the tail for the next bytesWritten.
      const QByteArray tail = frame.mid(int(written));
      m_outQueue.prepend(tail);
      ++m_outChunks;
      m_outBytes += tail.size();
      break;
    }
  }

  if (m_blocked) {
    const int pendingBytes = m_outBytes + int(m_socket->bytesToWrite());
    if (m_outChunks <= kResumeChunks && pendingBytes <= kResumeBytes) {
      m_blocked = false;
      scheduleWritable();
    }
  }
}

void BtLoopbackLink::scheduleWritable()
{
  if (m_writableScheduled) {
    return;
  }
  m_writableScheduled = true;
  // Queued so a caller inside sendChunk() cannot re-enter the relay's flush loop.
  QMetaObject::invokeMethod(
      this,
      [this] {
        m_writableScheduled = false;
        if (m_connected) {
          Q_EMIT writable();
        }
      },
      Qt::QueuedConnection
  );
}

void BtLoopbackLink::markConnected()
{
  if (m_connected) {
    return;
  }
  m_connected = true;
  LOG_DEBUG("bluetooth: loopback peer connected (%s)", qPrintable(describe()));
  Q_EMIT connected();
  scheduleWritable();
}

void BtLoopbackLink::markDisconnected(const QString &reason)
{
  m_connected = false;
  m_blocked = false;
  LOG_INFO("bluetooth: loopback link closed: %s", qPrintable(reason));
  Q_EMIT disconnected(reason);
}

void BtLoopbackLink::fail(const QString &message)
{
  LOG_ERR("bluetooth: %s", qPrintable(message));
  Q_EMIT errorOccurred(message);
  stop();
}

quint16 BtLoopbackLink::envPort()
{
  const QByteArray value = qgetenv("DESKFLOW_BT_LOOPBACK_PORT").trimmed();
  if (value.isEmpty()) {
    return 0;
  }

  bool ok = false;
  const uint port = value.toUInt(&ok);
  if (!ok || port > 65535) {
    LOG_WARN("bluetooth: ignoring invalid DESKFLOW_BT_LOOPBACK_PORT \"%s\"", value.constData());
    return 0;
  }
  return quint16(port);
}

int BtLoopbackLink::envChunkSize()
{
  const QByteArray value = qgetenv("DESKFLOW_BT_LOOPBACK_CHUNK").trimmed();
  if (value.isEmpty()) {
    return kDefaultChunkSize;
  }

  bool ok = false;
  const int size = value.toInt(&ok);
  if (!ok) {
    LOG_WARN("bluetooth: ignoring invalid DESKFLOW_BT_LOOPBACK_CHUNK \"%s\"", value.constData());
    return kDefaultChunkSize;
  }
  return clampChunkSize(size);
}

QByteArray BtLoopbackLink::encodeFrame(const QByteArray &chunk)
{
  QByteArray frame(kFrameHeaderSize + chunk.size(), '\0');
  qToLittleEndian<quint32>(quint32(chunk.size()), reinterpret_cast<uchar *>(frame.data()));
  if (!chunk.isEmpty()) {
    memcpy(frame.data() + kFrameHeaderSize, chunk.constData(), size_t(chunk.size()));
  }
  return frame;
}

} // namespace deskflow::net::bt
