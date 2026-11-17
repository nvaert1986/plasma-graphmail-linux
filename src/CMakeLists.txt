# SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# The mail cache and sync engine (phase 2): shared by the daemon and, read-only, by the UI
add_library(graphmailcore STATIC)
set_target_properties(graphmailcore PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_sources(graphmailcore PRIVATE
    graphmailcore/blobstore.cpp
    graphmailcore/updates.cpp
    graphmailcore/bodyfetcher.cpp
    graphmailcore/collectionsync.cpp
    graphmailcore/conflict.cpp
    graphmailcore/eventqueue.cpp
    graphmailcore/graphmapper.cpp
    graphmailcore/itemqueue.cpp
    graphmailcore/mailboxaccess.cpp
    graphmailcore/mailboxsettings.cpp
    graphmailcore/messageimport.cpp
    graphmailcore/pst/psttables.cpp
    graphmailcore/pst/pstalgorithms.cpp
    graphmailcore/pst/pstndb.cpp
    graphmailcore/pst/pstndbwriter.cpp
    graphmailcore/pst/pstltp.cpp
    graphmailcore/pst/pstltpwriter.cpp
    graphmailcore/pst/pstmessaging.cpp
    graphmailcore/pst/pstwriter.cpp
    graphmailcore/pst/mapigraph.cpp
    graphmailcore/pst/rtf.cpp
    graphmailcore/pst/mapiitems.cpp
    graphmailcore/pst/mapiitemsexport.cpp
    graphmailcore/cfb/cfb.cpp
    graphmailcore/msg/msgfile.cpp
    graphmailcore/mailboxworker.cpp
    graphmailcore/operationqueue.cpp
    graphmailcore/outboxengine.cpp
    graphmailcore/outgoing.cpp
    graphmailcore/reminders.cpp
    graphmailcore/searchquery.cpp
    graphmailcore/store.cpp
    graphmailcore/storeextra.cpp
)

ecm_qt_declare_logging_category(graphmailcore
    HEADER graphmail_debug.h
    IDENTIFIER GRAPHMAIL
    CATEGORY_NAME graphmail
    DEFAULT_SEVERITY Info
    DESCRIPTION "Mail cache and sync engine (plasma-graphmail-linux)"
    EXPORT GRAPHMAIL
)

target_include_directories(graphmailcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/graphmailcore ${CMAKE_CURRENT_BINARY_DIR})

# Test hooks (GRAPHMAIL_TEST_* variables) exist in this project's Debug builds only; packages use
# other build types
target_compile_definitions(graphmailcore PRIVATE $<$<CONFIG:Debug>:GRAPHMAIL_TESTING>)
if(GRAPHMAIL_UPDATE_CHECK)
    target_compile_definitions(graphmailcore PRIVATE GRAPHMAIL_UPDATE_CHECK)
endif()

target_link_libraries(graphmailcore PUBLIC
    GraphCore::GraphCore
    # Came through the old in-tree graphcore before phase 10
    Qt6::Concurrent
    Qt6::Sql
    PkgConfig::ZSTD
)

add_executable(graphmailctl graphmailctl/main.cpp)
target_link_libraries(graphmailctl PRIVATE GraphCore::GraphCore graphmailcore Qt6::DBus)

add_executable(graphmaild graphmaild/main.cpp graphmaild/daemon.cpp graphmaild/mailrunner.cpp)
target_link_libraries(graphmaild PRIVATE graphmailcore Qt6::DBus KF6::CoreAddons KF6::I18n KF6::Notifications)
target_compile_definitions(graphmaild PRIVATE TRANSLATION_DOMAIN="plasma-graphmail-linux")

# The app: Outlook-like window over the read-only caches (phase 3)
add_executable(plasma-graphmail-linux
    app/main.cpp
    app/attachmentpreview.cpp
    app/attachmentstrip.cpp
    app/avatar.cpp
    app/calendardata.cpp
    app/calendarexport.cpp
    app/calendaroptions.cpp
    app/calendarviews.cpp
    app/calendarsharingdialog.cpp
    app/categoriesdialog.cpp
    app/autorepliesdialog.cpp
    app/rulesdialog.cpp
    app/mailboxsettingsdialog.cpp
    app/templates.cpp
    app/quicksteps.cpp
    app/shell.cpp
    app/profilephotos.cpp
    app/printing.cpp
    app/mimeexport.cpp
    app/mimemapi.cpp
    app/pstexport.cpp
    app/pstimport.cpp
    app/categorylist.cpp
    app/calendarwindow.cpp
    app/cleanup.cpp
    app/composer.cpp
    app/coloraction.cpp
    app/pastedimages.cpp
    app/maileditor.cpp
    app/conditionalformat.cpp
    app/accesstext.cpp
    app/contactdialog.cpp
    app/contactswindow.cpp
    app/conversationmodel.cpp
    app/daemonclient.cpp
    app/eventdialog.cpp
    app/foldermodel.cpp
    app/icsdialog.cpp
    app/mailboxesdialog.cpp
    app/mailboxset.cpp
    app/mailto.cpp
    app/mailview.cpp
    app/mainwindow.cpp
    app/messagedelegate.cpp
    app/messagedetails.cpp
    app/messagedrag.cpp
    app/messagelistmodel.cpp
    app/messagewindow.cpp
    app/movedialog.cpp
    app/quote.cpp
    app/offlinedialog.cpp
    app/updatesdialog.cpp
    app/graphsynclink.cpp
    app/composercloud.cpp
    app/readingpane.cpp
    app/readreceipts.cpp
    app/recurrence.cpp
    app/recipientedit.cpp
    app/replyoptions.cpp
    app/unsubscribe.cpp
    app/vcard.cpp
    app/searchfolderdialog.cpp
    app/signaturedialog.cpp
    app/sweepdialog.cpp
    app/waitingwindow.cpp
    app/signatures.cpp
    app/taskswindow.cpp
    app/tnefdialog.cpp
)
qt_add_resources(plasma-graphmail-linux "ui"
    PREFIX "/kxmlgui5/plasma-graphmail-linux"
    BASE app
    FILES app/graphmailui.rc app/graphmailmessageui.rc app/graphmailcomposerui.rc app/graphmailcalendarui.rc app/graphmailcontactsui.rc app/graphmailtasksui.rc
)
target_link_libraries(plasma-graphmail-linux PRIVATE
    graphmailcore
    Qt6::Widgets
    Qt6::PrintSupport
    Qt6::WebEngineWidgets
    Qt6::OpenGLWidgets
    Qt6::DBus
    KF6::ConfigWidgets
    KF6::CoreAddons
    KF6::DBusAddons
    KF6::I18n
    KF6::IconThemes
    KF6::KIOGui
    KF6::KIOWidgets
    KF6::NotifyConfig
    KF6::WidgetsAddons
    KF6::XmlGui
    KF6::Parts
    KF6::Notifications
    KPim6::MimeTreeParserWidgets
    graphmail_kmime
    KPim6::Tnef
    KPim6::PimTextEdit
    KF6::TextEmoticonsWidgets
    KF6::Holidays
    KF6::CalendarCore
)
target_compile_definitions(plasma-graphmail-linux PRIVATE TRANSLATION_DOMAIN="plasma-graphmail-linux")

install(TARGETS graphmaild graphmailctl plasma-graphmail-linux ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
