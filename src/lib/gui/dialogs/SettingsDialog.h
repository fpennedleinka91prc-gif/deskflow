/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once
#include <QDialog>

#include "gui/config/ServerConfig.h"

class SettingsDialogButtonBox;

#ifdef DESKFLOW_BLUETOOTH
class QBluetoothDeviceDiscoveryAgent;
#endif

#if defined(DESKFLOW_BLUETOOTH) && defined(__APPLE__)
namespace deskflow::gui {
class MacBluetoothScanner;
}
#endif

namespace Ui {
class SettingsDialog;
}

class SettingsDialog : public QDialog
{
  Q_OBJECT

public:
  void extracted();
  SettingsDialog(QWidget *parent, const ServerConfig &serverConfig);
  ~SettingsDialog() override;

Q_SIGNALS:
  void requestRemoveAllSettings();

protected:
  void changeEvent(QEvent *e) override;

private:
  void initConnections() const;
  void regenCertificates();
  void browseCertificatePath();
  void browseLogPath();
  void setLogToFile(bool logToFile);
  void accept() override;
  bool isClientMode() const;
  void updateTlsControls();
  void updateTlsControlsEnabled();
  void updateBluetoothControls();
  void updateBluetoothHint();
#if defined(DESKFLOW_BLUETOOTH) && defined(__APPLE__)
  void startMacBluetoothScan();
#endif
  void startBluetoothScan();
  void stopBluetoothScan();
  void addBluetoothDevice(const QString &name, const QString &address);
  void onBluetoothDeviceSelected();
  void resetAllSettings();
  void updateText();

  /// @brief Load all settings.
  void loadFromConfig();

  /// @brief Updates the key length value based on the loaded file.
  void updateKeyLengthOnFile(const QString &path);

  /// @brief Enables controls when they should be.
  void updateControls();

  /// @brief updates the setting vaule for key size.
  void updateRequestedKeySize() const;

  /// @brief update if the log level warning is shown
  void logLevelChanged();

  /**
   * @brief isModified
   * @return true when any client settings in the gui do not match the stored settings values.
   */
  bool isModified() const;

  /**
   * @brief isDefault
   * @return true if all client settings match the default values
   */
  bool isDefault() const;

  /**
   * @brief Set the gui values to the defalut values for all client settings
   */
  void resetToDefault();

  /**
   * @brief setButtonBoxEnabledButtons
   * Enable / Disable the button box buttons based on the state of the gui
   */
  void setButtonBoxEnabledButtons() const;

  bool m_interfaceSetOnLoad = false;
  std::unique_ptr<Ui::SettingsDialog> ui;
  const ServerConfig &m_serverConfig;
  SettingsDialogButtonBox *m_buttonBox = nullptr;

#ifdef DESKFLOW_BLUETOOTH
  //! Device discovery for the "pick a device" list; created on the first scan.
  QBluetoothDeviceDiscoveryAgent *m_btAgent = nullptr;
#if defined(DESKFLOW_BLUETOOTH) && defined(__APPLE__)
  //! Qt has no macOS Bluetooth permission bridge, so the scan is native there.
  deskflow::gui::MacBluetoothScanner *m_macScanner = nullptr;
#endif
#endif
};
