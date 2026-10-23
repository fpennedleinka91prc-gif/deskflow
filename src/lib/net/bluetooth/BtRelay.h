/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "net/bluetooth/BtCodec.h"
#include "net/bluetooth/BtTypes.h"
#include "net/bluetooth/IBtLink.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

class QTcpServer;
class QTcpSocket;

namespace deskflow::net::bt {

//! Pumps the Deskflow byte stream between a local TCP socket and a Bluetooth link.
/*!
Deskflow's core is a classic select()/fd based engine (plus OpenSSL over those
fds), so instead of teaching it a new socket family we give it an ordinary
loopback TCP endpoint and tunnel that endpoint over Bluetooth:

- \c Server mode: the Deskflow server listens on \c 127.0.0.1:<port>.  When a
  Bluetooth peer connects, the relay dials that loopback port and shuttles
  bytes.  No inbound TCP port is exposed on the network at all.
- \c Client mode: the relay listens on an ephemeral loopback port and hands it
  to the Deskflow client, then connects to the Bluetooth peripheral.

Every Deskflow feature (screen edges, clipboard, hotkeys) works unchanged
because only the transport underneath the protocol is different.

The relay lives in its own thread with a Qt event loop; it owns the link and
the TCP socket so no locking is needed.
*/
class BtRelay : public QObject
{
  Q_OBJECT
public:
  //! \param serverPort Deskflow's loopback listen port (server mode); ignored in client mode.
  BtRelay(Mode mode, Config config, quint16 serverPort, QObject *parent = nullptr);
  ~BtRelay() override;

  //! Start listening/advertising.  Must be called from the relay thread.
  void start();

  //! Stop the link and drop the tunnel (relay thread).
  void stop();

  //! Local loopback port Deskflow should use (client mode: ready() reports it).
  quint16 localPort() const;

Q_SIGNALS:
  //! Transport is ready; \c port is the loopback port Deskflow talks to.
  void ready(quint16 port);

  //! Fatal setup failure (no Bluetooth adapter, port in use, ...).
  void failed(const QString &message);

  //! Non fatal progress/state change, forwarded to the log.
  void status(const QString &message);

private:
  void onLinkConnected();
  void onLinkDisconnected(const QString &reason);
  void onLinkChunk(const QByteArray &chunk);
  void onLinkWritable();
  void onLinkError(const QString &message);

  void onTcpNewConnection();
  void onTcpConnected();
  void onTcpReadyRead();
  void onTcpDisconnected();

  void connectTcp();
  void beginSession();
  //! Re-read the link's chunk size; the link only knows its MTU once connected.
  void updateChunkSize();
  void flushToLink();
  void flushPendingToTcp();
  void dropTcp(const QString &reason);
  void scheduleLinkRestart();

  Mode m_mode;
  Config m_config;
  quint16 m_serverPort = 0;
  quint16 m_localPort = 0;

  IBtLink *m_link = nullptr;
  QTcpServer *m_tcpServer = nullptr;
  QTcpSocket *m_tcpSocket = nullptr;
  BtCodec m_codec;
  QList<QByteArray> m_outbox;
  //! Bytes that arrived from the peer before the Deskflow side attached.
  /*!
  Both roles race: the peer starts talking the moment the link is up, while the
  Deskflow socket may still be connecting (server role) or may not have been
  accepted yet (client role).  Deskflow's protocol is server-speaks-first, so
  dropping those bytes loses the hello and the handshake times out.  They are
  held here until a Deskflow socket exists.
  */
  QByteArray m_pendingToTcp;
  bool m_started = false;
  bool m_running = false;
  bool m_linkBlocked = false;
};

} // namespace deskflow::net::bt
