// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.extras as PlasmaExtras

// The popup: the unread count and Open Graph Mail on top, then the reminders that are due, each
// with Snooze and Dismiss (as Outlook's reminder window), and the overdue ones from Exchange.
PlasmaExtras.Representation {
    id: full

    // The PlasmoidItem providing status and the commands
    required property var app

    collapseMarginsHint: true
    Layout.preferredWidth: Kirigami.Units.gridUnit * 24
    Layout.preferredHeight: Kirigami.Units.gridUnit * 22

    // When it starts, or the task's due date
    function when(item) {
        if (item.kind === "task") {
            return item.due ? i18n("Task, due %1", Qt.formatDate(new Date(item.due), Qt.locale().dateFormat(Locale.ShortFormat))) : i18n("Task");
        }
        const start = new Date(item.start);
        const minutes = Math.round((item.start - Date.now()) / 60000);
        const time = Qt.formatTime(start, Qt.locale().timeFormat(Locale.ShortFormat));
        if (minutes > 0 && minutes < 60) {
            return i18np("In 1 minute (%2)", "In %1 minutes (%2)", minutes, time);
        }
        if (minutes <= 0 && item.end > Date.now()) {
            return i18n("Now (started %1)", time);
        }
        return Qt.formatDateTime(start, Qt.locale().dateTimeFormat(Locale.ShortFormat));
    }

    header: PlasmaExtras.PlasmoidHeading {
        RowLayout {
            anchors.fill: parent
            spacing: Kirigami.Units.smallSpacing

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: 3
                    elide: Text.ElideRight
                    text: i18n("Graph Mail")
                }
                // The reminders are listed below: here only the mail
                PlasmaComponents.Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    opacity: 0.7
                    text: !full.app.daemonRunning ? i18n("Graph Mail is not running")
                        : (full.app.unread > 0 ? i18np("1 unread message", "%1 unread messages", full.app.unread) : i18n("No unread mail"))
                }
            }
            PlasmaComponents.ToolButton {
                icon.name: "mail-message-new"
                text: i18n("New Message")
                display: QQC2.AbstractButton.IconOnly
                enabled: full.app.daemonRunning
                onClicked: full.app.open("compose", null)
                PlasmaComponents.ToolTip.text: text
                PlasmaComponents.ToolTip.visible: hovered
            }
            PlasmaComponents.ToolButton {
                icon.name: "io.github.nvaert1986.graphmail"
                text: i18n("Open Graph Mail")
                onClicked: full.app.open("", null)
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Overdue reminders (events that are over), from Exchange's list: one line with Dismiss All
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.smallSpacing
            visible: full.app.overdueCount > 0
            Kirigami.Icon {
                source: "appointment-missed-symbolic"
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
            }
            PlasmaComponents.Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: i18np("1 overdue reminder", "%1 overdue reminders", full.app.overdueCount)
            }
            PlasmaComponents.Button {
                text: i18n("Dismiss")
                icon.name: "dialog-close"
                onClicked: full.app.dismissOverdue()
            }
        }

        PlasmaComponents.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            contentItem: ListView {
                id: list
                model: full.app.reminders
                spacing: Kirigami.Units.smallSpacing
                topMargin: Kirigami.Units.smallSpacing
                bottomMargin: Kirigami.Units.smallSpacing
                clip: true

                delegate: PlasmaComponents.ItemDelegate {
                    id: row
                    required property var modelData
                    width: ListView.view.width
                    // Click the reminder: open its event or task in Graph Mail
                    onClicked: full.app.open(modelData.kind, modelData)

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.smallSpacing
                        Kirigami.Icon {
                            source: row.modelData.kind === "task" ? "view-task" : "appointment-reminder"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            PlasmaComponents.Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                font.bold: true
                                text: row.modelData.title || i18n("(no subject)")
                            }
                            PlasmaComponents.Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                opacity: 0.7
                                text: (row.modelData.location ?? "").length > 0 ? i18nc("time, place", "%1, %2", full.when(row.modelData), row.modelData.location)
                                                                               : full.when(row.modelData)
                            }
                        }
                        PlasmaComponents.ToolButton {
                            id: snoozeButton
                            icon.name: "chronometer"
                            text: i18n("Snooze")
                            display: QQC2.AbstractButton.IconOnly
                            onClicked: snoozeMenu.popup()
                            PlasmaComponents.ToolTip.text: text
                            PlasmaComponents.ToolTip.visible: hovered

                            PlasmaComponents.Menu {
                                id: snoozeMenu
                                Repeater {
                                    model: [5, 10, 15, 30, 60]
                                    delegate: PlasmaComponents.MenuItem {
                                        required property int modelData
                                        text: modelData < 60 ? i18np("1 minute", "%1 minutes", modelData) : i18n("1 hour")
                                        onTriggered: full.app.snooze(row.modelData, modelData)
                                    }
                                }
                            }
                        }
                        PlasmaComponents.ToolButton {
                            icon.name: "dialog-close"
                            text: i18n("Dismiss")
                            display: QQC2.AbstractButton.IconOnly
                            onClicked: full.app.dismiss(row.modelData)
                            PlasmaComponents.ToolTip.text: text
                            PlasmaComponents.ToolTip.visible: hovered
                        }
                    }
                }

                PlasmaExtras.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit * 4
                    visible: list.count === 0
                    iconName: full.app.daemonRunning ? "appointment-reminder" : "io.github.nvaert1986.graphmail"
                    text: full.app.daemonRunning ? i18n("No reminders") : i18n("Graph Mail is not running")
                }
            }
        }

        // Several due: dismiss them all at once
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.smallSpacing
            visible: full.app.reminders.length > 1
            Item {
                Layout.fillWidth: true
            }
            PlasmaComponents.Button {
                text: i18n("Dismiss All")
                icon.name: "dialog-close"
                onClicked: full.app.dismissAll()
            }
        }
    }
}
