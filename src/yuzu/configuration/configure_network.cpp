// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QtConcurrent/QtConcurrent>
#include "common/settings.h"
#include "core/core.h"
#include "core/internal_network/network_interface.h"
#include "qt_common/config/uisettings.h"
#include "ui_configure_network.h"
#include "yuzu/configuration/configure_network.h"

ConfigureNetwork::ConfigureNetwork(const Core::System& system_, QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureNetwork>()), system{system_} {
    ui->setupUi(this);
    connect(ui->lan_online, &QCheckBox::clicked, this, [this](bool enabled) {
        if (enabled) {
            ui->airplane_mode->setChecked(false);
        }
    });
    for (const auto& iface : Network::GetAvailableNetworkInterfaces())
        ui->network_interface->addItem(QString::fromStdString(iface.name));
    this->SetConfiguration();
}

ConfigureNetwork::~ConfigureNetwork() = default;

void ConfigureNetwork::ApplyConfiguration() {
    Settings::values.network_interface = ui->network_interface->currentText().toStdString();
    Settings::values.airplane_mode = ui->airplane_mode->isChecked();
    Settings::values.lan_online = ui->lan_online->isChecked();
    UISettings::values.lan_online_tls = ui->lan_online_tls->isChecked();
}

void ConfigureNetwork::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureNetwork::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureNetwork::SetConfiguration() {
    const bool runtime_lock = !system.IsPoweredOn();
    auto const network_interface = Settings::values.network_interface.GetValue();
    auto const airplane_mode = Settings::values.airplane_mode.GetValue();
    ui->network_interface->setCurrentText(QString::fromStdString(network_interface));
    ui->network_interface->setEnabled(runtime_lock);
    ui->airplane_mode->setChecked(airplane_mode);
    ui->lan_online->setChecked(Settings::values.lan_online.GetValue());
    ui->lan_online->setEnabled(runtime_lock);
    ui->lan_online_tls->setChecked(UISettings::values.lan_online_tls.GetValue());
    ui->lan_online_tls->setEnabled(runtime_lock);
#ifndef __linux__
    ui->lan_online->setEnabled(false);
    ui->lan_online_tls->setEnabled(false);
#endif
}
