/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/bluetooth/BtTransport.h"

#include "base/Log.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include <memory>

using namespace deskflow::net::bt;

namespace {

//! Pick a loopback port that is free right now.
quint16 freeLoopbackPort()
{
  QTcpServer probe;
  if (!probe.listen(QHostAddress::LocalHost, 0)) {
    return 0;
  }
  const auto port = probe.serverPort();
  probe.close();
  return port;
}

//! Deterministic payload so a failure can be reproduced.
QByteArray makePayload(int size, quint32 seed)
{
  QRandomGenerator rng(seed);
  QByteArray payload(size, Qt::Uninitialized);
  for (int i = 0; i < size; ++i) {
    payload[i] = static_cast<char>(rng.bounded(256));
  }
  return payload;
}

//! Read exactly \c size bytes from \c socket, pumping the event loop meanwhile.
QByteArray readExactly(QTcpSocket *socket, int size)
{
  QByteArray received;
  received.reserve(size);
  QElapsedTimer timer;
  timer.start();
  while (received.size() < size && timer.elapsed() < 30000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (socket->bytesAvailable() > 0) {
      received.append(socket->read(size - received.size()));
    }
    if (socket->state() == QAbstractSocket::UnconnectedState && socket->bytesAvailable() == 0) {
      break;
    }
  }
  return received;
}

} // namespace

class BluetoothTransportTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void tunnelCarriesBytesBothWays();
  void transportReportsLoopbackPort();

private:
  //! The relay logs through the global logger, which the test has to provide.
  std::unique_ptr<Log> m_log;
};

void BluetoothTransportTests::initTestCase()
{
  m_log = std::make_unique<Log>();
  m_log->setFilter(QStringLiteral("FATAL"));
  // The tests drive the relay with the radio free backend.
  qputenv("DESKFLOW_BT_BACKEND", "loopback");
}

void BluetoothTransportTests::cleanupTestCase()
{
  m_log.reset();
}

//! Drive the relay end to end with the radio free loopback backend.
/*!
Two transports are started in one process: one in server mode, one in client
mode.  The server side dials a plain QTcpServer that stands in for the Deskflow
server's client listener, the client side is dialled by a plain QTcpSocket that
stands in for the Deskflow client.  Everything in between (link framing, credit
based flow control, the relay and its thread) is the code under test.
*/
void BluetoothTransportTests::tunnelCarriesBytesBothWays()
{
  const auto peerPort = freeLoopbackPort();
  QVERIFY(peerPort != 0);
  qputenv("DESKFLOW_BT_LOOPBACK_PORT", QByteArray::number(peerPort));
  // Small chunks so the credit window and the framing are exercised.
  qputenv("DESKFLOW_BT_LOOPBACK_CHUNK", "64");

  QTcpServer deskflowListener;
  QVERIFY(deskflowListener.listen(QHostAddress::LocalHost, 0));

  Config config;
  config.localName = QStringLiteral("unit-test-server");
  config.peerName = QStringLiteral("unit-test-server");

  QString error;
  BtTransport server(Mode::Server, config);
  QVERIFY2(server.start(deskflowListener.serverPort(), &error), qPrintable(error));

  BtTransport client(Mode::Client, config);
  QVERIFY2(client.start(0, &error), qPrintable(error));
  QVERIFY(client.localPort() != 0);

  // The Deskflow client side of the tunnel.
  QTcpSocket clientSide;
  clientSide.connectToHost(QHostAddress::LocalHost, client.localPort());
  QVERIFY(clientSide.waitForConnected(10000));

  // The relay dials the listener once the peer link is up.
  QTRY_VERIFY_WITH_TIMEOUT(deskflowListener.hasPendingConnections(), 15000);
  QScopedPointer<QTcpSocket> deskflowSide(deskflowListener.nextPendingConnection());
  QVERIFY(deskflowSide);
  QTRY_COMPARE_WITH_TIMEOUT(deskflowSide->state(), QAbstractSocket::ConnectedState, 15000);

  // Server to client.
  const auto toClient = makePayload(200000, 0x5eed1234);
  QCOMPARE(deskflowSide->write(toClient), static_cast<qint64>(toClient.size()));
  QVERIFY(deskflowSide->waitForBytesWritten(10000));
  QCOMPARE(readExactly(&clientSide, toClient.size()), toClient);

  // Client to server, twice the size, to cross the credit window more than once.
  const auto toServer = makePayload(400000, 0x0badc0de);
  QCOMPARE(clientSide.write(toServer), static_cast<qint64>(toServer.size()));
  QVERIFY(clientSide.waitForBytesWritten(10000));
  QCOMPARE(readExactly(deskflowSide.data(), toServer.size()), toServer);

  clientSide.disconnectFromHost();
  client.stop();
  server.stop();
}

//! The client mode transport must expose a usable loopback port, server mode
//! must report the port it was told to expect the Deskflow listener on.
void BluetoothTransportTests::transportReportsLoopbackPort()
{
  const auto peerPort = freeLoopbackPort();
  QVERIFY(peerPort != 0);
  qputenv("DESKFLOW_BT_LOOPBACK_PORT", QByteArray::number(peerPort));

  Config config;
  config.localName = QStringLiteral("unit-test-port");

  BtTransport client(Mode::Client, config);
  QString error;
  QVERIFY2(client.start(0, &error), qPrintable(error));
  QVERIFY(client.isRunning());
  QVERIFY(client.localPort() != 0);

  QTcpSocket probe;
  probe.connectToHost(QHostAddress::LocalHost, client.localPort());
  QVERIFY(probe.waitForConnected(10000));
  probe.abort();

  client.stop();
  QVERIFY(!client.isRunning());
}

QTEST_MAIN(BluetoothTransportTests)
#include "BluetoothTransportTests.moc"
