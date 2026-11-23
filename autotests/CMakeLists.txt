# SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# The Graph core's own tests (accounts, sign-in, the Graph client) live in plasma-graphcore-linux

ecm_add_tests(
    blobstoretest.cpp
    storetest.cpp
    collectionsynctest.cpp
    operationstest.cpp
    outboxtest.cpp
    messageimporttest.cpp
    eventqueuetest.cpp
    itemqueuetest.cpp
    mailboxaccesstest.cpp
    mailboxsettingstest.cpp
    pstalgorithmstest.cpp
    pstltptest.cpp
    pstmessagingtest.cpp
    pstwritertest.cpp
    mapigraphtest.cpp
    msgtest.cpp
    rtftest.cpp
    mapiitemstest.cpp
    fuzztest.cpp
    mailboxworkertest.cpp
    LINK_LIBRARIES graphmailcore Qt6::Test
)

# The app's models, with a temporary cache (QStandardPaths test mode)
ecm_add_test(appmodelstest.cpp
    ../src/app/calendardata.cpp
    ../src/app/calendarexport.cpp
    ../src/app/cleanup.cpp
    ../src/app/conditionalformat.cpp
    ../src/app/calendaroptions.cpp
    ../src/app/calendarviews.cpp
    ../src/app/recurrence.cpp
    ../src/app/conversationmodel.cpp
    ../src/app/foldermodel.cpp
    ../src/app/avatar.cpp
    ../src/app/mailboxset.cpp
    ../src/app/messagelistmodel.cpp
    ../src/app/searchfolderdialog.cpp
    ../src/app/categorylist.cpp
    ../src/app/daemonclient.cpp
    ../src/app/rulesdialog.cpp
    ../src/app/mimeexport.cpp
    ../src/app/mailto.cpp
    ../src/app/messagedetails.cpp
    ../src/app/messagedrag.cpp
    ../src/app/readreceipts.cpp
    ../src/app/recipientedit.cpp
    ../src/app/replyoptions.cpp
    ../src/app/sweepdialog.cpp
    ../src/app/movedialog.cpp
    ../src/app/unsubscribe.cpp
    ../src/app/vcard.cpp
    ../src/app/mimemapi.cpp
    ../src/app/accesstext.cpp
    ../src/app/maileditor.cpp
    ../src/app/signaturedialog.cpp
    ../src/app/signatures.cpp
    ../src/app/coloraction.cpp
    ../src/app/pastedimages.cpp
    ../src/app/spelllanguage.cpp
    ../src/app/fontfamilybox.cpp
    TEST_NAME appmodelstest
    LINK_LIBRARIES graphmailcore KF6::I18n KF6::ConfigCore KF6::WidgetsAddons KF6::Holidays KF6::CalendarCore KF6::XmlGui graphmail_kmime KPim6::PimTextEdit KF6::SonnetCore KF6::SonnetUi Qt6::Widgets Qt6::DBus Qt6::Test
)
target_include_directories(appmodelstest PRIVATE ${CMAKE_SOURCE_DIR}/src/app)

# These share QStandardPaths' test-mode directories (~/.qttest), so they must not run at the same time
set_tests_properties(appmodelstest PROPERTIES RESOURCE_LOCK qttest-paths)

# Replies and forwards: recipients, subjects, the quote header
ecm_add_test(quotetest.cpp ../src/app/quote.cpp
    TEST_NAME quotetest
    LINK_LIBRARIES graphmailcore KF6::I18n Qt6::Gui Qt6::Test
)
target_include_directories(quotetest PRIVATE ${CMAKE_SOURCE_DIR}/src/app)

# PST and .msg export: MIME to MAPI properties, and back through a PST to Graph
ecm_add_test(mimemapitest.cpp ../src/app/mimemapi.cpp
    TEST_NAME mimemapitest
    LINK_LIBRARIES graphmailcore graphmail_kmime Qt6::Gui Qt6::Test
)
target_include_directories(mimemapitest PRIVATE ${CMAKE_SOURCE_DIR}/src/app ${CMAKE_SOURCE_DIR}/src/graphmailcore)

# The phase 8 stress test: run by hand, not by ctest (build/bin/storebench [messages] [directory])
add_executable(storebench storebench.cpp)
target_link_libraries(storebench PRIVATE graphmailcore)

# Every PST the writer makes must pass the independent checker
target_compile_definitions(pstwritertest PRIVATE PST_CHECKER="${CMAKE_SOURCE_DIR}/.dev/tools/pstcheck.py")
target_compile_definitions(msgtest PRIVATE MSG_CHECKER="${CMAKE_SOURCE_DIR}/.dev/tools/msgcheck.py")

# Dates and time zones must not depend on the machine's zone: once more, west of UTC
add_test(NAME mapiitemstest-newyork COMMAND mapiitemstest)
set_tests_properties(mapiitemstest-newyork PROPERTIES ENVIRONMENT "TZ=America/New_York")
