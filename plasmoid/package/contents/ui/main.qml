// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import org.kde.kirigami as Kirigami
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.plasmoid
import org.kde.plasma.workspace.components as WorkspaceComponents
import org.kde.plasma.workspace.dbus as DBus

// Graph Mail in the system tray: the unread count of the own Inboxes on a mail icon, and the
// reminders that are due (Snooze, Dismiss), as Graph Sync's cloud icon shows its state.
// Everything comes from graphmaild over D-Bus (Command, the same pattern as Graph Sync's tray).
PlasmoidItem {
    id: root

    readonly property string service: "io.github.nvaert1986.GraphMail"
    readonly property bool daemonRunning: watcher.registered
    property var status: ({})

    readonly property int unread: status.unread ?? 0
    readonly property var reminders: status.reminders ?? []
    readonly property var overdue: status.overdue ?? []
    readonly property int overdueCount: overdue.reduce((n, o) => n + (o.count ?? 0), 0)
    readonly property bool needsAttention: reminders.length > 0 || overdueCount > 0

    readonly property string statusText: {
        if (!daemonRunning) {
            return i18n("Graph Mail is not running");
        }
        const parts = [];
        parts.push(unread > 0 ? i18np("1 unread message", "%1 unread messages", unread) : i18n("No unread mail"));
        if (reminders.length > 0) {
            parts.push(i18np("1 reminder", "%1 reminders", reminders.length));
        }
        if (overdueCount > 0) {
            parts.push(i18np("1 overdue reminder", "%1 overdue reminders", overdueCount));
        }
        return parts.join(" · ");
    }

    // The module hands back a pending-reply object: the method's return value is in
    // reply.value.value, an error in reply.error.message
    function unwrap(reply) {
        const value = reply?.value;
        return (value !== null && typeof value === "object") ? value.value : value;
    }

    function command(object, onResult) {
        // The one-argument asyncCall and its reply's finished signal (Plasma 6.3 has no asyncCall
        // with result callbacks); a typed string argument, as Graph Sync's tray does
        const reply = DBus.SessionBus.asyncCall({
            service: service,
            path: "/GraphMail",
            iface: "io.github.nvaert1986.GraphMail",
            member: "Command",
            arguments: [new DBus.string(JSON.stringify(object))]
        });
        reply.finished.connect(() => {
            if (reply.isError) {
                console.warn("plasma-graphmail-linux:", reply.error?.message);
                return;
            }
            if (onResult) {
                const text = unwrap(reply);
                try {
                    onResult(JSON.parse(text));
                } catch (e) {
                    console.warn("plasma-graphmail-linux: unexpected reply", text);
                }
            }
        });
    }

    function refresh() {
        if (!daemonRunning) {
            status = {};
            return;
        }
        command({ command: "status" }, result => root.status = result);
    }

    // kind: "" (the main window), event, task, compose or calendar
    function open(kind, item) {
        command({ command: "open", kind: kind, account: item?.account ?? "", mailbox: item?.mailbox ?? "", id: item?.id ?? 0 });
        root.expanded = false;
    }

    function snooze(item, minutes) {
        command({ command: "snooze", account: item.account, mailbox: item.mailbox, kind: item.kind, id: item.id, minutes: minutes }, () => refresh());
    }

    function dismiss(item) {
        command({ command: "dismiss", account: item.account, mailbox: item.mailbox, kind: item.kind, id: item.id }, () => refresh());
    }

    function dismissAll() {
        for (const item of reminders) {
            command({ command: "dismiss", account: item.account, mailbox: item.mailbox, kind: item.kind, id: item.id });
        }
        refresh();
    }

    function dismissOverdue() {
        for (const o of overdue) {
            command({ command: "dismissOverdue", account: o.account, mailbox: o.mailbox });
        }
        refresh();
    }

    // Right-click > Configure Graph Mail... (ConfigGeneral.qml)
    readonly property string reminderIcon: Plasmoid.configuration.reminderIcon
    readonly property bool showUnreadCount: Plasmoid.configuration.showUnreadCount
    readonly property string envelope: unread > 0 ? "mail-unread-new-symbolic" : "mail-message-symbolic"

    // Monochrome, like the other icons in the tray; a bell instead of the envelope while
    // reminders are due, unless the user chose the envelope (with or without a small bell)
    Plasmoid.icon: needsAttention && reminderIcon === "bell" ? "appointment-reminder-symbolic" : envelope
    Plasmoid.status: needsAttention ? PlasmaCore.Types.NeedsAttentionStatus : (daemonRunning ? PlasmaCore.Types.ActiveStatus : PlasmaCore.Types.PassiveStatus)
    Plasmoid.title: i18n("Graph Mail")
    toolTipMainText: i18n("Graph Mail")
    toolTipSubText: statusText

    // The mail icon with the unread count on it, as mail programs show it in the tray
    compactRepresentation: MouseArea {
        id: compact
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        onClicked: mouse => {
            if (mouse.button === Qt.MiddleButton) {
                root.open("", null);
            } else {
                root.expanded = !root.expanded;
            }
        }

        Kirigami.Icon {
            id: icon
            anchors.fill: parent
            source: Plasmoid.icon
            active: compact.containsMouse
        }

        WorkspaceComponents.BadgeOverlay {
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            visible: root.showUnreadCount && root.unread > 0
            text: root.unread > 99 ? "99+" : String(root.unread)
            icon: icon
        }

        // "The envelope with a small bell": the bell in the opposite corner of the count
        Kirigami.Icon {
            anchors.top: parent.top
            anchors.left: parent.left
            width: Math.round(parent.width * 0.55)
            height: width
            visible: root.needsAttention && root.reminderIcon === "badge"
            source: "appointment-reminder-symbolic"
            color: Kirigami.Theme.highlightColor
            isMask: true
        }
    }

    // Right-click menu
    Plasmoid.contextualActions: [
        PlasmaCore.Action {
            text: i18n("Open Graph Mail")
            icon.name: "io.github.nvaert1986.graphmail"
            onTriggered: root.open("", null)
        },
        PlasmaCore.Action {
            text: i18n("New Message")
            icon.name: "mail-message-new"
            onTriggered: root.open("compose", null)
        },
        PlasmaCore.Action {
            text: i18n("Calendar")
            icon.name: "view-calendar"
            onTriggered: root.open("calendar", null)
        }
    ]

    fullRepresentation: FullRepresentation {
        app: root
    }

    DBus.DBusServiceWatcher {
        id: watcher
        busType: DBus.BusType.Session
        watchedService: root.service
        onRegisteredChanged: root.refresh()
    }

    Timer {
        // Cheap to ask locally: the daemon answers from memory
        interval: root.expanded ? 2000 : 10000
        running: root.daemonRunning
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refresh()
    }

    onExpandedChanged: () => {
        if (root.expanded) {
            refresh();
        }
    }
}
