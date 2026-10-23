/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/bluetooth/BtTransport.h"

#include "base/Log.h"
#include "common/Settings.h"
#include "net/bluetooth/BtRelay.h"

#include <QHostInfo>
#include <QSemaphore>
#include <QThread>

namespace deskflow::net::bt {

struct BtTransport::Impl
{
  QThread thread;
  BtRelay *relay = nullptr;
};

BtTransport::BtTransport(Mode mode, Config config) : m_mode(mode), m_config(std::move(config))
{
}

BtTransport::~BtTransport()
{
  stop();
}

bool BtTransport::start(quint16 serverPort, QString *error, int timeoutMs)
{
  if (m_impl != nullptr) {
    return m_running;
  }

  auto *impl = new Impl;
  impl->thread.setObjectName(QStringLiteral("DeskflowBluetooth"));
  impl->relay = new BtRelay(m_mode, m_config, serverPort);
  impl->relay->moveToThread(&impl->thread);

  // The relay reports readiness synchronously from its own thread while we
  // block here, so the connections are direct and guarded by the semaphore.
  QSemaphore ready;
  bool started = false;
  quint16 port = 0;
  QString failure;

  const auto readyConnection = QObject::connect(
      impl->relay, &BtRelay::ready, impl->relay,
      [&ready, &started, &port](quint16 p) {
        started = true;
        port = p;
        ready.release();
      },
      Qt::DirectConnection
  );
  const auto failedConnection = QObject::connect(
      impl->relay, &BtRelay::failed, impl->relay,
      [&ready, &failure](const QString &message) {
        failure = message;
        ready.release();
      },
      Qt::DirectConnection
  );
  QObject::connect(
      impl->relay, &BtRelay::status, impl->relay,
      [](const QString &message) { LOG_INFO("bluetooth: %s", message.toUtf8().constData()); },
      Qt::DirectConnection
  );

  QObject::connect(&impl->thread, &QThread::started, impl->relay, &BtRelay::start);
  impl->thread.start();

  if (!ready.tryAcquire(1, timeoutMs)) {
    failure = QStringLiteral("timed out after %1 ms waiting for the Bluetooth transport").arg(timeoutMs);
  }

  // The handlers above hold references to locals, so they must not outlive us.
  QObject::disconnect(readyConnection);
  QObject::disconnect(failedConnection);

  if (!started) {
    impl->thread.quit();
    impl->thread.wait(3000);
    delete impl->relay;
    delete impl;
    if (error != nullptr) {
      *error = failure.isEmpty() ? QStringLiteral("Bluetooth transport failed to start") : failure;
    }
    return false;
  }

  // Failures after startup (link drops, protocol errors) are reported, not fatal.
  QObject::connect(
      impl->relay, &BtRelay::failed, impl->relay,
      [](const QString &message) { LOG_ERR("bluetooth: %s", message.toUtf8().constData()); }, Qt::DirectConnection
  );

  m_impl = impl;
  m_running = true;
  LOG_INFO("bluetooth: transport ready on 127.0.0.1:%d", port);
  return true;
}

quint16 BtTransport::localPort() const
{
  return m_impl != nullptr && m_impl->relay != nullptr ? m_impl->relay->localPort() : 0;
}

void BtTransport::stop()
{
  if (m_impl == nullptr) {
    return;
  }

  if (m_impl->thread.isRunning()) {
    QMetaObject::invokeMethod(m_impl->relay, [relay = m_impl->relay] { relay->stop(); }, Qt::BlockingQueuedConnection);
    m_impl->thread.quit();
    m_impl->thread.wait(5000);
  }

  delete m_impl->relay;
  delete m_impl;
  m_impl = nullptr;
  m_running = false;
}

bool BtTransport::isRunning() const
{
  return m_impl != nullptr && m_running;
}

bool BtTransport::enabledBySettings()
{
  if (qEnvironmentVariableIsSet("DESKFLOW_BT_ENABLED")) {
    return qEnvironmentVariableIntValue("DESKFLOW_BT_ENABLED") != 0;
  }
  return Settings::value(Settings::Bluetooth::Enabled).toBool();
}

Config BtTransport::configFromSettings()
{
  Config config;
  config.localName = Settings::value(Settings::Bluetooth::Name).toString();
  if (config.localName.isEmpty()) {
    config.localName = QHostInfo::localHostName();
  }
  config.peerAddress = Settings::value(Settings::Bluetooth::Peer).toString();
  config.peerName = config.peerAddress;

  const int chunkSize = Settings::value(Settings::Bluetooth::ChunkSize).toInt();
  if (chunkSize > 0) {
    config.chunkSize = chunkSize;
  }
  const int credits = Settings::value(Settings::Bluetooth::InitialCredits).toInt();
  if (credits > 0) {
    config.initialCredits = credits;
  }

  return config;
}

} // namespace deskflow::net::bt
