/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QCommandLineOption>
/**
 * @brief The CoreArgs class
 * This class contains args for the CoreArgParser
 */
struct CoreArgs
{
  inline static const auto helpOption = QCommandLineOption({"h", "help"}, "Display Help on the command line");
  inline static const auto versionOption = QCommandLineOption({"v", "version"}, "Display version information");
  inline static const auto multiInstanceOption =
      QCommandLineOption("new-instance", "Skip the check for a running instance, always makes a new instance");
  inline static const auto configOption =
      QCommandLineOption({"s", "settings"}, "override configuration file to use", "configFile");

  // Bluetooth transport (experimental)
  inline static const auto bluetoothOption = QCommandLineOption(
      "bluetooth", "Use the Bluetooth transport instead of a network connection (experimental)"
  );
  inline static const auto btPeerOption =
      QCommandLineOption("bt-peer", "Bluetooth peer address or advertised name", "address|name");
  inline static const auto btNameOption =
      QCommandLineOption("bt-name", "Local name advertised over Bluetooth", "local name");
  inline static const auto btBackendOption =
      QCommandLineOption("bt-backend", "Bluetooth backend: qt, darwin, rfcomm (classic) or loopback (test)", "backend");

  inline static const auto options = {
      helpOption,
      versionOption,
      multiInstanceOption,
      configOption,
      bluetoothOption,
      btPeerOption,
      btNameOption,
      btBackendOption,
  };
};
