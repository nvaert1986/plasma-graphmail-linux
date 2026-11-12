# SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

ecm_add_tests(
    authtest.cpp
    accountstest.cpp
    graphutiltest.cpp
    graphclienttest.cpp
    LINK_LIBRARIES graphcore Qt6::Test
)

ecm_add_tests(
    blobstoretest.cpp
    storetest.cpp
    collectionsynctest.cpp
    operationstest.cpp
    outboxtest.cpp
    eventqueuetest.cpp
    itemqueuetest.cpp
    mailboxaccesstest.cpp
    mailboxsettingstest.cpp
    mailboxworkertest.cpp
    LINK_LIBRARIES graphmailcore Qt6::Test
)

# The app's models, with a temporary cache (QStandardPaths test mode)
ecm_add_test(appmodelstest.cpp
    ../src/app/calendardata.cpp
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
    ../src/app/movedialog.cpp
    ../src/app/accesstext.cpp
    TEST_NAME appmodelstest
    LINK_LIBRARIES graphmailcore KF6::I18n KF6::ConfigCore KF6::WidgetsAddons Qt6::Widgets Qt6::DBus Qt6::Test
)
target_include_directories(appmodelstest PRIVATE ${CMAKE_SOURCE_DIR}/src/app)

# These share QStandardPaths' test-mode directories (~/.qttest), so they must not run at the same time
set_tests_properties(accountstest authtest appmodelstest PROPERTIES RESOURCE_LOCK qttest-paths)

# Replies and forwards: recipients, subjects, the quote header
ecm_add_test(quotetest.cpp ../src/app/quote.cpp
    TEST_NAME quotetest
    LINK_LIBRARIES graphmailcore KF6::I18n Qt6::Gui Qt6::Test
)
target_include_directories(quotetest PRIVATE ${CMAKE_SOURCE_DIR}/src/app)

# The phase 8 stress test: run by hand, not by ctest (build/bin/storebench [messages] [directory])
add_executable(storebench storebench.cpp)
target_link_libraries(storebench PRIVATE graphmailcore)
