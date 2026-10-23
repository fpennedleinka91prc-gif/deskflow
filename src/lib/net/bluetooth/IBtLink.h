/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "net/bluetooth/BtTypes.h"

#include <QByteArray>
#include <QObject>
#include <QString>

namespace deskflow::net::bt {

//! A reliable, ordered, message based pipe to a Bluetooth peer.
/*!
Implementations live in their own thread with a Qt event loop (see \c BtRelay).
All signals are emitted from that thread; \c BtRelay connects to them with
direct connections because it shares the thread.

A link is deliberately dumb: it moves opaque chunks.  Stream semantics,
framing, credit based flow control and error detection all live in \c BtCodec
so that every backend (BLE GATT today, RFCOMM/L2CAP tomorrow) behaves the same.
*/
class IBtLink : public QObject
{
  Q_OBJECT
public:
  explicit IBtLink(QObject *parent = nullptr) : QObject(parent)
  {
  }
  ~IBtLink() override = default;

  //! Advertise as a peripheral and wait for a central (server side).
  virtual void startServer(const Config &config) = 0;

  //! Scan for and connect to a peripheral (client side).
  virtual void startClient(const Config &config) = 0;

  //! Stop advertising/scanning and drop the current connection.
  virtual void stop() = 0;

  virtual bool isConnected() const = 0;

  //! Largest payload a single chunk may carry (MTU - chunk header).
  virtual int maxChunkSize() const = 0;

  //! Queue one chunk.  Returns false when the backend cannot take more right now.
  /*!
  When this returns false the caller must wait for \c writable() before
  offering more chunks; the chunk is *not* consumed.
  */
  virtual bool sendChunk(const QByteArray &chunk) = 0;

  //! Human readable description of the current state, used for logging.
  virtual QString describe() const = 0;

Q_SIGNALS:
  //! A peer is connected and the GATT characteristics are ready.
  void connected();

  //! The peer went away; \c reason is for logging only.
  void disconnected(const QString &reason);

  //! One chunk arrived from the peer (already reassembled by the backend).
  void chunkReceived(const QByteArray &chunk);

  //! The backend can accept more chunks after \c sendChunk returned false.
  void writable();

  //! A fatal backend error; the link is no longer usable.
  void errorOccurred(const QString &message);
};

} // namespace deskflow::net::bt
