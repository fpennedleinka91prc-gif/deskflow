/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/bluetooth/BtRfcommWinLink.h"

#include "base/Log.h"

#ifdef _WIN32

#include <QTimer>

#include <cstring>

#include <winsock2.h>
#include <ws2bth.h>
#include <bluetoothapis.h>

namespace deskflow::net::bt {

namespace {

//! The service GUID identifies the Deskflow serial port to SDP clients.
constexpr GUID kServiceGuid = {0x3f2a7c1e, 0x9b64, 0x4d58, {0x8a, 0x11, 0x6c, 0x0d, 0x5e, 0x7b, 0x9f, 0x20}};

//! Chunk size for RFCOMM payloads.
constexpr int kMaxChunk = 512;

//! Backlog for the listening socket.
constexpr int kListenBacklog = 2;

bool winsockReady()
{
  static bool ready = false;
  static bool attempted = false;
  if (attempted) {
    return ready;
  }
  attempted = true;

  WSADATA data{};
  ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  if (!ready) {
    LOG_ERR("bluetooth/rfcomm: WSAStartup failed");
  }
  return ready;
}

} // namespace

class BtRfcommWinLink::Private
{
public:
  BtRfcommWinLink *q = nullptr;
  Mode mode = Mode::Client;
  Config config;

  SOCKET listenSocket = INVALID_SOCKET;
  SOCKET dataSocket = INVALID_SOCKET;
  CSADDR_INFO serviceInfo{};
  SOCKADDR_BTH serviceAddress{};

  bool running = false;
  bool connected = false;
  bool published = false;
  QString peerLabel;

  QTimer *retryTimer = nullptr;
  QTimer *acceptTimer = nullptr;
  QTimer *readTimer = nullptr;

  bool publishService();
  void acceptLoop();
  void readLoop();
  void connectToServer();
  void markConnected();
  void markDisconnected(const QString &reason);
  void cleanup();
  BTH_ADDR parseAddress(const QString &text) const;
};

BtRfcommWinLink::BtRfcommWinLink(QObject *parent) : IBtLink(parent), d(new Private)
{
  d->q = this;
  d->retryTimer = new QTimer(this);
  d->retryTimer->setSingleShot(true);
  connect(d->retryTimer, &QTimer::timeout, this, [this] {
    if (!d->running || d->connected) {
      return;
    }
    if (d->mode == Mode::Server) {
      d->acceptLoop();
    } else {
      d->connectToServer();
    }
  });

  d->acceptTimer = new QTimer(this);
  d->acceptTimer->setInterval(500);
  connect(d->acceptTimer, &QTimer::timeout, this, [this] { d->acceptLoop(); });

  d->readTimer = new QTimer(this);
  d->readTimer->setInterval(10);
  connect(d->readTimer, &QTimer::timeout, this, [this] { d->readLoop(); });
}

BtRfcommWinLink::~BtRfcommWinLink()
{
  stop();
  delete d;
}

BTH_ADDR BtRfcommWinLink::Private::parseAddress(const QString &text) const
{
  if (text.isEmpty()) {
    return 0;
  }
  // Accept both "00:11:22:33:44:55" and "001122334455" forms.
  QString digits = text;
  digits.remove(QLatin1Char(':'));
  digits.remove(QLatin1Char('-'));
  bool ok = false;
  const auto value = digits.toULongLong(&ok, 16);
  return ok ? static_cast<BTH_ADDR>(value) : 0;
}

bool BtRfcommWinLink::Private::publishService()
{
  SOCKET socket = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
  if (socket == INVALID_SOCKET) {
    LOG_ERR("bluetooth/rfcomm: cannot create an RFCOMM socket (0x%x)", WSAGetLastError());
    return false;
  }

  SOCKADDR_BTH local{};
  local.addressFamily = AF_BTH;
  local.btAddr = 0; // any local adapter
  local.port = BT_PORT_ANY;

  if (::bind(socket, reinterpret_cast<SOCKADDR *>(&local), sizeof(local)) == SOCKET_ERROR) {
    LOG_ERR("bluetooth/rfcomm: bind failed (0x%x)", WSAGetLastError());
    ::closesocket(socket);
    return false;
  }

  // Ask the Bluetooth stack to accept incoming RFCOMM connections.
  if (::listen(socket, kListenBacklog) == SOCKET_ERROR) {
    LOG_ERR("bluetooth/rfcomm: listen failed (0x%x)", WSAGetLastError());
    ::closesocket(socket);
    return false;
  }

  // Publish the service so the peer's SDP query finds it.
  SOCKADDR_BTH bound{};
  int length = sizeof(bound);
  if (::getsockname(socket, reinterpret_cast<SOCKADDR *>(&bound), &length) == SOCKET_ERROR) {
    LOG_WARN("bluetooth/rfcomm: getsockname failed (0x%x)", WSAGetLastError());
  }

  CSADDR_INFO info{};
  info.LocalAddr.iSockaddrLength = sizeof(bound);
  info.LocalAddr.lpSockaddr = reinterpret_cast<SOCKADDR *>(&bound);
  info.RemoteAddr.iSockaddrLength = sizeof(bound);
  info.RemoteAddr.lpSockaddr = reinterpret_cast<SOCKADDR *>(&bound);
  info.iSocketType = SOCK_STREAM;
  info.iProtocol = BTHPROTO_RFCOMM;
  serviceInfo = info;

  // Description and service name shown to the other computer.
  WSAQUERYSET query{};
  query.dwNameSpace = NS_BTH;
  query.dwSize = sizeof(query);
  query.lpszServiceInstanceName = const_cast<LPWSTR>(L"Deskflow");
  query.lpszComment = const_cast<LPWSTR>(L"Deskflow keyboard and mouse sharing");
  query.lpServiceClassId = const_cast<GUID *>(&kServiceGuid);
  query.dwNumberOfCsAddrs = 1;
  query.lpcsaBuffer = &serviceInfo;

  if (::WSASetService(&query, RNRSERVICE_REGISTER, 0) == SOCKET_ERROR) {
    LOG_WARN("bluetooth/rfcomm: WSASetService failed (0x%x)", WSAGetLastError());
    // Not fatal: the socket still listens, the peer just will not see it in SDP.
  } else {
    published = true;
  }

  listenSocket = socket;
  u_long nonBlocking = 1;
  ::ioctlsocket(listenSocket, FIONBIO, &nonBlocking);
  LOG_INFO(
      "bluetooth/rfcomm: listening on RFCOMM channel %d, waiting for the paired computer",
      static_cast<int>(bound.port)
  );
  return true;
}

void BtRfcommWinLink::Private::acceptLoop()
{
  if (!running || connected || listenSocket == INVALID_SOCKET) {
    return;
  }

  SOCKET client = ::accept(listenSocket, nullptr, nullptr);
  if (client == INVALID_SOCKET) {
    return;
  }
  u_long nonBlocking = 1;
  ::ioctlsocket(client, FIONBIO, &nonBlocking);

  dataSocket = client;
  if (acceptTimer != nullptr) {
    acceptTimer->stop();
  }
  markConnected();
}

void BtRfcommWinLink::Private::readLoop()
{
  if (!connected || dataSocket == INVALID_SOCKET) {
    return;
  }

  char buffer[4096];
  const int received = ::recv(dataSocket, buffer, sizeof(buffer), 0);
  if (received <= 0) {
    if (received == 0) {
      markDisconnected(QStringLiteral("the other computer closed the Bluetooth link"));
    } else {
      const int err = WSAGetLastError();
      if (err != WSAEWOULDBLOCK) {
        markDisconnected(QStringLiteral("the Bluetooth link failed (%1)").arg(err));
      }
    }
    return;
  }

  const QByteArray chunk(buffer, received);
  Q_EMIT q->chunkReceived(chunk);
}

void BtRfcommWinLink::Private::connectToServer()
{
  if (!running || connected) {
    return;
  }

  const BTH_ADDR target = parseAddress(config.peerAddress);
  if (target == 0) {
    LOG_WARN("bluetooth/rfcomm: no peer address configured, expected the other computer's Bluetooth address");
    if (retryTimer != nullptr) {
      retryTimer->start(config.retryIntervalMs > 0 ? config.retryIntervalMs : 2000);
    }
    return;
  }

  SOCKADDR_BTH remote{};
  remote.addressFamily = AF_BTH;
  remote.btAddr = target;
  remote.port = 0; // let SDP resolve the serial port channel
  remote.serviceClassId = kServiceGuid;

  SOCKET socket = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
  if (socket == INVALID_SOCKET) {
    LOG_ERR("bluetooth/rfcomm: cannot create an RFCOMM socket (0x%x)", WSAGetLastError());
    if (retryTimer != nullptr) {
      retryTimer->start(2000);
    }
    return;
  }

  if (::connect(socket, reinterpret_cast<SOCKADDR *>(&remote), sizeof(remote)) == SOCKET_ERROR) {
    LOG_WARN("bluetooth/rfcomm: connect failed (0x%x)", WSAGetLastError());
    ::closesocket(socket);
    if (retryTimer != nullptr) {
      retryTimer->start(config.retryIntervalMs > 0 ? config.retryIntervalMs : 2000);
    }
    return;
  }

  dataSocket = socket;
  markConnected();
}

void BtRfcommWinLink::Private::markConnected()
{
  if (connected) {
    return;
  }
  connected = true;
  if (retryTimer != nullptr) {
    retryTimer->stop();
  }
  if (readTimer != nullptr) {
    readTimer->start();
  }
  LOG_INFO("bluetooth/rfcomm: the Bluetooth link is up");
  Q_EMIT q->connected();
  Q_EMIT q->writable();
}

void BtRfcommWinLink::Private::markDisconnected(const QString &reason)
{
  if (!connected) {
    return;
  }
  connected = false;
  if (readTimer != nullptr) {
    readTimer->stop();
  }
  Q_EMIT q->disconnected(reason);
  cleanup();
  if (running && retryTimer != nullptr) {
    retryTimer->start(config.retryIntervalMs > 0 ? config.retryIntervalMs : 2000);
  }
}

void BtRfcommWinLink::Private::cleanup()
{
  if (dataSocket != INVALID_SOCKET) {
    ::closesocket(dataSocket);
    dataSocket = INVALID_SOCKET;
  }
  if (listenSocket != INVALID_SOCKET) {
    ::closesocket(listenSocket);
    listenSocket = INVALID_SOCKET;
  }
  if (published) {
    WSAQUERYSET query{};
    query.dwNameSpace = NS_BTH;
    query.lpServiceClassId = const_cast<GUID *>(&kServiceGuid);
    ::WSASetService(&query, RNRSERVICE_DELETE, 0);
    published = false;
  }
}

void BtRfcommWinLink::startServer(const Config &config)
{
  if (!winsockReady()) {
    Q_EMIT errorOccurred(QStringLiteral("Winsock could not be started"));
    return;
  }

  d->mode = Mode::Server;
  d->config = config;
  d->running = true;

  if (!d->publishService()) {
    Q_EMIT errorOccurred(QStringLiteral("Could not open the Bluetooth serial port on this computer"));
    return;
  }

  d->acceptTimer->start();
}

void BtRfcommWinLink::startClient(const Config &config)
{
  if (!winsockReady()) {
    Q_EMIT errorOccurred(QStringLiteral("Winsock could not be started"));
    return;
  }

  d->mode = Mode::Client;
  d->config = config;
  d->running = true;
  d->peerLabel = config.peerAddress;
  d->connectToServer();
}

void BtRfcommWinLink::stop()
{
  d->running = false;
  d->connected = false;
  if (d->retryTimer != nullptr) {
    d->retryTimer->stop();
  }
  if (d->acceptTimer != nullptr) {
    d->acceptTimer->stop();
  }
  if (d->readTimer != nullptr) {
    d->readTimer->stop();
  }
  d->cleanup();
}

bool BtRfcommWinLink::isConnected() const
{
  return d->connected;
}

int BtRfcommWinLink::maxChunkSize() const
{
  return kMaxChunk;
}

bool BtRfcommWinLink::sendChunk(const QByteArray &chunk)
{
  if (!d->connected || d->dataSocket == INVALID_SOCKET || chunk.isEmpty()) {
    return false;
  }

  const int sent = ::send(d->dataSocket, chunk.constData(), chunk.size(), 0);
  if (sent <= 0) {
    LOG_WARN("bluetooth/rfcomm: send failed (0x%x)", WSAGetLastError());
    return false;
  }

  QTimer::singleShot(0, this, [this] { Q_EMIT writable(); });
  return true;
}

QString BtRfcommWinLink::describe() const
{
  const QString state = d->connected ? QStringLiteral("connected") : QStringLiteral("waiting");
  if (d->mode == Mode::Server) {
    return QStringLiteral("classic Bluetooth server (%1)").arg(state);
  }
  return QStringLiteral("classic Bluetooth -> %1 (%2)")
      .arg(d->peerLabel.isEmpty() ? QStringLiteral("paired computer") : d->peerLabel, state);
}

#else // !_WIN32

namespace deskflow::net::bt {

class BtRfcommWinLink::Private
{
public:
  BtRfcommWinLink *q = nullptr;
};

BtRfcommWinLink::BtRfcommWinLink(QObject *parent) : IBtLink(parent), d(new Private)
{
  d->q = this;
}

BtRfcommWinLink::~BtRfcommWinLink()
{
  delete d;
}

void BtRfcommWinLink::startServer(const Config &config)
{
  Q_UNUSED(config);
  Q_EMIT errorOccurred(QStringLiteral("Classic Bluetooth on Windows is only available in a Windows build"));
}

void BtRfcommWinLink::startClient(const Config &config)
{
  Q_UNUSED(config);
  Q_EMIT errorOccurred(QStringLiteral("Classic Bluetooth on Windows is only available in a Windows build"));
}

void BtRfcommWinLink::stop()
{
}

bool BtRfcommWinLink::isConnected() const
{
  return false;
}

int BtRfcommWinLink::maxChunkSize() const
{
  return 180;
}

bool BtRfcommWinLink::sendChunk(const QByteArray &chunk)
{
  Q_UNUSED(chunk);
  return false;
}

QString BtRfcommWinLink::describe() const
{
  return QStringLiteral("classic Bluetooth (unavailable)");
}

#endif // _WIN32

} // namespace deskflow::net::bt
