// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kcmutils as KCMUtils
import org.kde.kirigami as Kirigami

// Right-click > Configure Graph Mail...: how the tray icon looks; the pop-ups themselves are
// KDE's notification settings for Graph Mail (one button away)
KCMUtils.SimpleKCM {
    property string cfg_reminderIcon
    property bool cfg_showUnreadCount

    Kirigami.FormLayout {
        QQC2.ButtonGroup {
            id: reminderGroup
        }
        QQC2.RadioButton {
            Kirigami.FormData.label: i18n("When reminders are due:")
            text: i18n("Show a bell instead of the envelope")
            QQC2.ButtonGroup.group: reminderGroup
            checked: cfg_reminderIcon === "bell"
            onToggled: if (checked) cfg_reminderIcon = "bell"
        }
        QQC2.RadioButton {
            text: i18n("Show the envelope with a small bell")
            QQC2.ButtonGroup.group: reminderGroup
            checked: cfg_reminderIcon === "badge"
            onToggled: if (checked) cfg_reminderIcon = "badge"
        }
        QQC2.RadioButton {
            text: i18n("Show the envelope only")
            QQC2.ButtonGroup.group: reminderGroup
            checked: cfg_reminderIcon === "envelope"
            onToggled: if (checked) cfg_reminderIcon = "envelope"
        }

        Item {
            Kirigami.FormData.isSection: true
        }
        QQC2.CheckBox {
            Kirigami.FormData.label: i18n("Unread mail:")
            text: i18n("Show the number of unread messages on the icon")
            checked: cfg_showUnreadCount
            onToggled: cfg_showUnreadCount = checked
        }

        Item {
            Kirigami.FormData.isSection: true
        }
        QQC2.Button {
            Kirigami.FormData.label: i18n("Pop-ups and sounds:")
            text: i18n("Configure Notifications…")
            icon.name: "preferences-desktop-notification-bell"
            onClicked: KCMUtils.KCMLauncher.openSystemSettings("kcm_notifications", ["--notifyrc", "plasma-graphmail-linux"])
        }
        QQC2.Label {
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 22
            wrapMode: Text.WordWrap
            font: Kirigami.Theme.smallFont
            text: i18n("For each kind (new mail, reminders, tasks, new versions) whether Graph Mail shows a pop-up or plays a sound.")
        }
    }
}
