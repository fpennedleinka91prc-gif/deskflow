/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/bluetooth/BtRelay.h"

#include "base/Log.h"
#include "net/bluetooth/BtLinkFactory.h"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace deskflow::net::bt {

namespace {
//! How long to wait before dialling the Deskflow listener again (server mode).
constexpr int kTcpRetryMs = 500;
//! Upper bound for bytes held while the Deskflow socket is still coming up.
constexpr qsizetype kMaxPendingToTcp = 4 * 1024 * 1024;
} // namespace

BtRelay::BtRelay(Mode mode, Config config, quint16 serverPort, QObject *parent)
    : QObject(parent),
      m_mode(mode),
      m_config(std::move(config)),
      m_serverPort(serverPort),
      m_codec(m_config.chunkSize > 0 ? m_config.chunkSize : 180, m_config.initialCredits)
{
}

BtRelay::~BtRelay() = default;

quint16 BtRelay::localPort() const
{
  return m_mode == Mode::Client ? m_localPort : m_serverPort;
}

void BtRelay::start()
{
  if (m_started) {
    return;
  }
  m_started = true;
  m_running = true;

  m_link = BtLinkFactory::create(this);
  if (m_link == nullptr) {
    Q_EMIT failed(tr("No Bluetooth backend is available in this build"));
    return;
  }

  connect(m_link, &IBtLink::connected, this, &BtRelay::onLinkConnected);
  connect(m_link, &IBtLink::disconnected, this, &BtRelay::onLinkDisconnected);
  connect(m_link, &IBtLink::chunkReceived, this, &BtRelay::onLinkChunk);
  connect(m_link, &IBtLink::writable, this, &BtRelay::onLinkWritable);
  connect(m_link, &IBtLink::errorOccurred, this, &BtRelay::onLinkError);

  Q_EMIT status(tr("Bluetooth backend: %1").arg(BtLinkFactory::backendName()));

  if (m_mode == Mode::Client) {
    m_tcpServer = new QTcpServer(this);
    if (!m_tcpServer->listen(QHostAddress::LocalHost, 0)) {
      Q_EMIT failed(
          tr("Unable to open the local Bluetooth tunnel on 127.0.0.1: %1").arg(m_tcpServer->errorString())
      );
      return;
    }
    m_localPort = m_tcpServer->serverPort();
    connect(m_tcpServer, &QTcpServer::newConnection, this, &BtRelay::onTcpNewConnection);
    Q_EMIT status(tr("Local tunnel listening on 127.0.0.1:%1").arg(m_localPort));
  }

  updateChunkSize();

  if (m_mode == Mode::Server) {
    m_link->startServer(m_config);
    Q_EMIT ready(m_serverPort);
  } else {
    m_link->startClient(m_config);
    Q_EMIT ready(m_localPort);
  }
}

void BtRelay::stop()
{
  m_running = false;
  if (m_link != nullptr) {
    m_link->stop();
  }
  dropTcp(tr("Bluetooth transport stopping"));
  if (m_tcpServer != nullptr) {
    m_tcpServer->close();
  }
}

void BtRelay::onLinkConnected()
{
  // The link knows its real MTU now, so re-frame with the size it can carry.
  updateChunkSize();
  // A new link means a new session: anything still pending belongs to the old one.
  m_pendingToTcp.clear();
  Q_EMIT status(tr("Bluetooth peer connected (%1)").arg(m_link->describe()));
  if (m_mode == Mode::Server) {
    // The Deskflow server is already listening; dial it as a normal client.
    connectTcp();
  } else {
    beginSession();
  }
}

void BtRelay::onLinkDisconnected(const QString &reason)
{
  Q_EMIT status(tr("Bluetooth peer disconnected: %1").arg(reason));
  dropTcp(reason);
  m_codec.reset();
  m_outbox.clear();
  m_linkBlocked = false;
  if (m_mode == Mode::Client) {
    scheduleLinkRestart();
  }
}

void BtRelay::onLinkError(const QString &message)
{
  Q_EMIT failed(message);
  dropTcp(message);
}

void BtRelay::onLinkChunk(const QByteArray &chunk)
{
  const QByteArray payload = m_codec.decode(chunk);
  if (m_codec.hasError()) {
    Q_EMIT failed(m_codec.errorString());
    dropTcp(m_codec.errorString());
    return;
  }

  if (!payload.isEmpty()) {
    if (m_tcpSocket != nullptr) {
      m_tcpSocket->write(payload);
    } else if (m_pendingToTcp.size() < kMaxPendingToTcp) {
      // The Deskflow side is not attached yet; keep the bytes for it.
      m_pendingToTcp.append(payload);
    } else {
      LOG_WARN("bluetooth: dropping tunnel bytes, no Deskflow socket is attached");
    }
  }

  if (m_codec.peerClosed()) {
    Q_EMIT status(tr("Bluetooth peer closed the tunnel"));
    dropTcp(tr("Bluetooth peer closed the tunnel"));
    return;
  }

  // Decoding may have granted credit, so try to push pending data now.
  flushToLink();
}

void BtRelay::onLinkWritable()
{
  m_linkBlocked = false;
  // An MTU update (or a reconnect) can change the usable size at any time.
  updateChunkSize();
  flushToLink();
}

void BtRelay::updateChunkSize()
{
  if (m_link == nullptr) {
    return;
  }
  const int size = m_link->maxChunkSize();
  if (size > 0 && size != m_codec.chunkSize()) {
    m_codec.setChunkSize(size);
  }
}

void BtRelay::onTcpNewConnection()
{
  QTcpSocket *socket = m_tcpServer->nextPendingConnection();
  if (socket == nullptr) {
    return;
  }
  if (m_tcpSocket != nullptr) {
    // Only one Deskflow client may use the tunnel at a time.
    socket->abort();
    socket->deleteLater();
    return;
  }

  m_tcpSocket = socket;
  connect(socket, &QTcpSocket::connected, this, &BtRelay::onTcpConnected);
  connect(socket, &QTcpSocket::readyRead, this, &BtRelay::onTcpReadyRead);
  connect(socket, &QTcpSocket::disconnected, this, &BtRelay::onTcpDisconnected);
  Q_EMIT status(tr("Deskflow client attached to the local tunnel"));

  if (m_link != nullptr && m_link->isConnected()) {
    beginSession();
  }
}

void BtRelay::onTcpConnected()
{
  beginSession();
}

void BtRelay::flushPendingToTcp()
{
  if (m_pendingToTcp.isEmpty()) {
    return;
  }
  if (m_tcpSocket == nullptr) {
    return;
  }
  m_tcpSocket->write(m_pendingToTcp);
  m_pendingToTcp.clear();
}

void BtRelay::onTcpReadyRead()
{
  if (m_tcpSocket == nullptr) {
    return;
  }
  const QByteArray bytes = m_tcpSocket->readAll();
  if (bytes.isEmpty()) {
    return;
  }
  m_codec.queueStream(bytes);
  if (m_codec.hasError()) {
    Q_EMIT failed(m_codec.errorString());
    dropTcp(m_codec.errorString());
    return;
  }
  flushToLink();
}

void BtRelay::onTcpDisconnected()
{
  dropTcp(tr("Deskflow closed the tunnel"));
  if (m_mode == Mode::Server && m_link != nullptr && m_link->isConnected()) {
    // The server is gone; there is nothing left to tunnel.
    m_link->stop();
  }
}

void BtRelay::connectTcp()
{
  if (m_tcpSocket != nullptr) {
    return;
  }

  auto *socket = new QTcpSocket(this);
  m_tcpSocket = socket;
  connect(socket, &QTcpSocket::connected, this, &BtRelay::onTcpConnected);
  connect(socket, &QTcpSocket::readyRead, this, &BtRelay::onTcpReadyRead);
  connect(socket, &QTcpSocket::disconnected, this, &BtRelay::onTcpDisconnected);
  connect(socket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
    // The Deskflow listener may not be up yet; retry shortly.
    const QString reason = m_tcpSocket != nullptr ? m_tcpSocket->errorString() : QString();
    dropTcp(reason);
    if (m_running && m_link != nullptr && m_link->isConnected()) {
      QTimer::singleShot(kTcpRetryMs, this, [this] {
        if (m_running && m_tcpSocket == nullptr && m_link != nullptr && m_link->isConnected()) {
          connectTcp();
        }
      });
    }
  });

  socket->connectToHost(QHostAddress::LocalHost, m_serverPort);
}

void BtRelay::beginSession()
{
  if (m_link == nullptr || !m_link->isConnected() || m_tcpSocket == nullptr) {
    return;
  }

  // The peer may already have spoken while our Deskflow socket was connecting.
  flushPendingToTcp();

  m_codec.reset();
  m_outbox.clear();
  m_linkBlocked = false;
  // The handshake must be the first chunk the peer sees.
  m_outbox.append(m_codec.helloChunk());
  flushToLink();
}

void BtRelay::flushToLink()
{
  if (m_link == nullptr || !m_link->isConnected() || m_linkBlocked) {
    return;
  }

  for (;;) {
    if (m_outbox.isEmpty()) {
      m_outbox = m_codec.takeChunks();
      if (m_outbox.isEmpty()) {
        return;
      }
    }

    const QByteArray chunk = m_outbox.takeFirst();
    if (!m_link->sendChunk(chunk)) {
      m_outbox.prepend(chunk);
      m_linkBlocked = true;
      return;
    }
  }
}

void BtRelay::dropTcp(const QString &reason)
{
  if (m_tcpSocket == nullptr) {
    return;
  }

  QTcpSocket *socket = m_tcpSocket;
  m_tcpSocket = nullptr;
  disconnect(socket, nullptr, this, nullptr);
  socket->abort();
  socket->deleteLater();
  LOG_DEBUG("bluetooth: dropped local tunnel: %s", reason.toUtf8().constData());
}

void BtRelay::scheduleLinkRestart()
{
  if (!m_running) {
    return;
  }

  QTimer::singleShot(m_config.retryIntervalMs, this, [this] {
    if (!m_running || m_link == nullptr || m_link->isConnected()) {
      return;
    }
    Q_EMIT status(tr("Reconnecting to the Bluetooth peer"));
    m_link->startClient(m_config);
  });
}

} // namespace deskflow::net::bt
