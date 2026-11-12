cmake_minimum_required(VERSION 3.12)
project(EZ4Connectpro VERSION 2.0.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(NIGHTLY_BUILD    "Set if built nightly.")

if(NIGHTLY_BUILD)
    set(PROJ_VER "${GIT_HASH}-nightly")
elseif(DEFINED REL_VER)
    set(PROJ_VER "${REL_VER}")
else()
    set(PROJ_VER "${PROJECT_VERSION}")
endif()

set(PROJ_AUTHOR "HuoXiaolin")
add_compile_definitions(PROJ_VER="${PROJ_VER}" PROJ_AUTHOR="${PROJ_AUTHOR}")
message("PROJ_VER=${PROJ_VER}")

find_package(Qt6 6.5 COMPONENTS
    Core
    Concurrent
    Gui
    Widgets
    Network
    Svg
    Core5Compat
    WebSockets
    REQUIRED
)


set(SOURCE_FILES
    main.cpp
    presentation/main/mainwindow.cpp
    presentation/main/workspace.cpp
    presentation/main/startupsettings.cpp
    presentation/main/traycontroller.cpp
    presentation/main/mainwindowcoordinator.cpp
    presentation/coordinators/authdialogcoordinator.cpp
    presentation/coordinators/connectionuicontroller.cpp
    application/applicationlogger.cpp
    application/commandlineoptions.cpp
    application/coreprocess.h
    application/connectionsession.cpp
    application/profileservice.cpp
    application/vpnprofile.cpp
    application/settingsmigrator.cpp
    application/systemproxysession.cpp
    application/defaultsettings.cpp
    infrastructure/coreprocess/corecommandbuilder.cpp
    infrastructure/coreprocess/consoleoutputdecoder.cpp
    infrastructure/coreprocess/coreexecutable.cpp
    infrastructure/coreprocess/devicetrust.cpp
    infrastructure/coreprocess/coreoutputbuffer.cpp
    infrastructure/coreprocess/coreoutputparser.cpp
    infrastructure/browser/browserssosession.cpp
    infrastructure/browser/temporarybrowserssosession.cpp
    core/connectionsessionstate.cpp
    infrastructure/logging/applicationlogfile.cpp
    infrastructure/platform/autostart.cpp
    infrastructure/platform/privileges.cpp
    infrastructure/platform/portableruntime.cpp
    infrastructure/platform/platformtunreservation.cpp
    infrastructure/settings/settingsprofileloader.cpp
    infrastructure/storage/applicationpaths.cpp
    infrastructure/update/updatechecker.cpp
    infrastructure/platform/platformsystemproxybackend.cpp
    infrastructure/coreprocess/zjuconnectprocess.cpp
    presentation/presentationhelpers.cpp
    presentation/dialogs/extrasettingwindow/extrasettingwindow.cpp
    presentation/dialogs/settingwindow/settingwindow.cpp
    presentation/dialogs/vpnprofiledialog/vpnprofiledialog.cpp
    presentation/dialogs/loginwindow/loginwindow.cpp
    presentation/dialogs/sudowindow/sudowindow.cpp
    presentation/dialogs/authinfowindow/authinfowindow.cpp
    presentation/dialogs/configurationguidedialog/configurationguidedialog.cpp
    presentation/dialogs/graphcaptchawindow/graphcaptchawindow.cpp
    presentation/dialogs/graphcaptchawindow/captchacanvas.cpp
    infrastructure/settings/profilemanager.cpp
    resource.qrc
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Ensure UI edits invalidate their consumers on the first incremental Ninja build.
set_property(SOURCE presentation/main/mainwindow.cpp presentation/main/workspace.cpp APPEND
    PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/presentation/main/mainwindow.ui")

if (NOT WIN32)
    list(APPEND SOURCE_FILES infrastructure/platform/systemproxyfunctions.cpp)
endif()

if (WIN32)
    message("Building for Windows")

    enable_language("RC")

    set(WINDOWS_FILE_VERSION "0,0,0,0")
    if(PROJ_VER MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        set(WINDOWS_FILE_VERSION "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0")
    endif()
    set(WINDOWS_FILE_DESCRIPTION "EZ4Connectpro")
    set(WINDOWS_ORIGINAL_FILENAME "EZ4Connectpro.exe")
    set(WINDOWS_COPYRIGHT "EZ4Connectpro by ${PROJ_AUTHOR}. Based on EZ4Connect by Chenx Dust. GPL-3.0.")
    configure_file(resource/windows.rc.in EZ4Connectpro.rc @ONLY)
    set(WINDOWS_FILE_DESCRIPTION "EZ4Connectpro · 运行文件管理")
    set(WINDOWS_ORIGINAL_FILENAME "EZ4Connectpro-windows-x64.exe")
    configure_file(resource/windows.rc.in EZ4Connectpro.Launcher.rc @ONLY)
    set(WINDOWS_FILE_DESCRIPTION "EZ4Connectpro · VPN 连接核心")
    set(WINDOWS_ORIGINAL_FILENAME "zju-connect.exe")
    set(WINDOWS_COPYRIGHT "ZJU Connect by Mythologyli. Windows integration by ${PROJ_AUTHOR}. AGPL-3.0.")
    configure_file(resource/windows.rc.in EZ4Connectpro.Core.rc @ONLY)

    add_executable(${PROJECT_NAME} WIN32
        ${SOURCE_FILES}
        ${CMAKE_CURRENT_BINARY_DIR}/EZ4Connectpro.rc
    )

    set(PLATFORM_LIBS wininet shell32 shlwapi cfgmgr32)
elseif(APPLE)
    message("Building for MacOS")
    
    set(MACOSX_BUNDLE_ICON_FILE icon.icns)
    
    set(app_icon_macos ${CMAKE_CURRENT_SOURCE_DIR}/resource/icon.icns)
    set_source_files_properties(${app_icon_macos} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

    add_executable(${PROJECT_NAME} MACOSX_BUNDLE
        ${SOURCE_FILES}
        ${app_icon_macos}
    )

    set_target_properties(${PROJECT_NAME}
        PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resource/Info.plist")
    find_library(CORE_SERVICES_FRAMEWORK CoreServices)
    set(PLATFORM_LIBS ${CORE_SERVICES_FRAMEWORK})
else()
    add_executable(${PROJECT_NAME}
        ${SOURCE_FILES}
    )

    install(
        TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION bin
    )
endif()

set(QAPPLICATION_CLASS QApplication CACHE STRING "Inheritance class for SingleApplication")

add_subdirectory(singleapplication)

target_link_libraries(${PROJECT_NAME}
        Qt::Core
        Qt::Concurrent
        Qt::Gui
        Qt::Widgets
        Qt::Network
        Qt::Svg
        Qt::Core5Compat
        Qt::WebSockets
        SingleApplication::SingleApplication
        ${PLATFORM_LIBS}
)

include(CTest)

if(BUILD_TESTING)
    if(WIN32)
        add_executable(windows_desktop_check scripts/windows-qa/desktop-check.cpp)
        target_link_libraries(windows_desktop_check gdiplus ws2_32 version)
        if(MINGW)
            target_link_options(windows_desktop_check PRIVATE -municode -static -static-libgcc -static-libstdc++)
        endif()

        set(STARTUP_TEST_SOURCES ${SOURCE_FILES})
        list(REMOVE_ITEM STARTUP_TEST_SOURCES main.cpp
            infrastructure/platform/privileges.cpp infrastructure/platform/autostart.cpp)
        add_executable(startupintegration_test tests/startupintegration_test.cpp ${STARTUP_TEST_SOURCES})
        target_link_libraries(startupintegration_test Qt::Core Qt::Concurrent Qt::Gui Qt::Widgets
            Qt::Network Qt::Svg Qt::Core5Compat Qt::WebSockets SingleApplication::SingleApplication ${PLATFORM_LIBS})
        add_executable(startup_mock_core tests/startup_mock_core.cpp)
        target_link_libraries(startup_mock_core Qt::Core Qt::Network)
        set_target_properties(startup_mock_core PROPERTIES OUTPUT_NAME "zju-connect")
        set_target_properties(startupintegration_test startup_mock_core PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/startup-qa")
        add_dependencies(startupintegration_test startup_mock_core)
        foreach(scenario auto empty disabled conflict elevate denied shutdown)
            add_test(NAME startup_${scenario}_test COMMAND startupintegration_test --autostart --scenario ${scenario})
            set_tests_properties(startup_${scenario}_test PROPERTIES ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
        endforeach()
        foreach(scenario manual ui)
            add_test(NAME startup_${scenario}_test COMMAND startupintegration_test --scenario ${scenario})
            set_tests_properties(startup_${scenario}_test PROPERTIES ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")
        endforeach()
    endif()

    add_executable(traycontroller_test
        tests/traycontroller_test.cpp
        presentation/main/traycontroller.cpp
        application/vpnprofile.cpp
        resource.qrc
    )
    target_link_libraries(traycontroller_test Qt::Core Qt::Gui Qt::Widgets)
    add_test(NAME traycontroller_test COMMAND traycontroller_test)
    set_tests_properties(traycontroller_test PROPERTIES ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")

    add_executable(applicationlogger_test
        tests/applicationlogger_test.cpp
        application/applicationlogger.cpp
    )
    target_include_directories(applicationlogger_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(applicationlogger_test Qt::Core)
    add_test(NAME applicationlogger_test COMMAND applicationlogger_test)

    add_executable(applicationlogfile_test
        tests/applicationlogfile_test.cpp
        infrastructure/logging/applicationlogfile.cpp
    )
    target_include_directories(applicationlogfile_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(applicationlogfile_test Qt::Core)
    add_test(NAME applicationlogfile_test COMMAND applicationlogfile_test)

    add_executable(corecommandbuilder_test
        tests/corecommandbuilder_test.cpp
        infrastructure/coreprocess/corecommandbuilder.cpp
    )
    target_include_directories(corecommandbuilder_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(corecommandbuilder_test Qt::Core)
    add_test(NAME corecommandbuilder_test COMMAND corecommandbuilder_test)

    add_executable(coreoutputparser_test
        tests/coreoutputparser_test.cpp
        infrastructure/coreprocess/coreoutputparser.cpp
    )
    target_include_directories(coreoutputparser_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(coreoutputparser_test Qt::Core)
    add_test(NAME coreoutputparser_test COMMAND coreoutputparser_test)

    add_executable(browserssosession_test
        tests/browserssosession_test.cpp
        infrastructure/browser/browserssosession.cpp
        infrastructure/browser/temporarybrowserssosession.cpp
    )
    target_link_libraries(browserssosession_test Qt::Core Qt::Network Qt::WebSockets ${PLATFORM_LIBS})
    add_test(NAME browserssosession_test COMMAND browserssosession_test)

    add_executable(coreoutputbuffer_test
        tests/coreoutputbuffer_test.cpp
        infrastructure/coreprocess/coreoutputbuffer.cpp
    )
    target_include_directories(coreoutputbuffer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(coreoutputbuffer_test Qt::Core)
    add_test(NAME coreoutputbuffer_test COMMAND coreoutputbuffer_test)

    add_executable(settingsprofileloader_test
        tests/settingsprofileloader_test.cpp
        infrastructure/settings/settingsprofileloader.cpp
        application/vpnprofile.cpp
    )
    target_include_directories(settingsprofileloader_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(settingsprofileloader_test Qt::Core)
    add_test(NAME settingsprofileloader_test COMMAND settingsprofileloader_test)

    add_executable(connectionsessionstate_test
        tests/connectionsessionstate_test.cpp
        core/connectionsessionstate.cpp
    )
    target_include_directories(connectionsessionstate_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(connectionsessionstate_test Qt::Core)
    add_test(NAME connectionsessionstate_test COMMAND connectionsessionstate_test)

    add_executable(connectionsession_test
        tests/connectionsession_test.cpp
        application/coreprocess.h
        application/connectionsession.cpp
        core/connectionsessionstate.cpp
    )
    target_include_directories(connectionsession_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(connectionsession_test Qt::Core)
    add_test(NAME connectionsession_test COMMAND connectionsession_test)

    add_executable(systemproxysession_test
        tests/systemproxysession_test.cpp
        application/systemproxysession.cpp
    )
    target_include_directories(systemproxysession_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(systemproxysession_test Qt::Core Qt::Concurrent)
    add_test(NAME systemproxysession_test COMMAND systemproxysession_test)

    add_executable(updatechecker_test
        tests/updatechecker_test.cpp
        infrastructure/update/updatechecker.cpp
    )
    target_include_directories(updatechecker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(updatechecker_test Qt::Core Qt::Network)
    add_test(NAME updatechecker_test COMMAND updatechecker_test)

    add_executable(settingsmigrator_test
        tests/settingsmigrator_test.cpp
        application/settingsmigrator.cpp
        application/defaultsettings.cpp
    )
    target_include_directories(settingsmigrator_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(settingsmigrator_test Qt::Core)
    add_test(NAME settingsmigrator_test COMMAND settingsmigrator_test)

    add_executable(profileservice_test
        tests/profileservice_test.cpp
        application/profileservice.cpp
        application/vpnprofile.cpp
        application/defaultsettings.cpp
    )
    target_include_directories(profileservice_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(profileservice_test Qt::Core)
    add_test(NAME profileservice_test COMMAND profileservice_test)

    add_executable(profilemanager_test
        tests/profilemanager_test.cpp
        infrastructure/settings/profilemanager.cpp
        infrastructure/storage/applicationpaths.cpp
    )
    target_include_directories(profilemanager_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(profilemanager_test Qt::Core)
    add_test(NAME profilemanager_test COMMAND profilemanager_test)

    add_executable(configurationguidedialog_test
        tests/configurationguidedialog_test.cpp
        presentation/dialogs/configurationguidedialog/configurationguidedialog.cpp
        presentation/dialogs/authinfowindow/authinfowindow.cpp
        infrastructure/coreprocess/consoleoutputdecoder.cpp
        infrastructure/coreprocess/coreexecutable.cpp
    )
    target_include_directories(configurationguidedialog_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(configurationguidedialog_test
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Core5Compat
    )
    add_test(NAME configurationguidedialog_test COMMAND configurationguidedialog_test)
    set_tests_properties(configurationguidedialog_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(graphcaptchawindow_test
        tests/graphcaptchawindow_test.cpp
        presentation/dialogs/graphcaptchawindow/graphcaptchawindow.cpp
        presentation/dialogs/graphcaptchawindow/captchacanvas.cpp
    )
    target_include_directories(graphcaptchawindow_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(graphcaptchawindow_test Qt::Core Qt::Gui Qt::Widgets)
    add_test(NAME graphcaptchawindow_test COMMAND graphcaptchawindow_test)
    set_tests_properties(graphcaptchawindow_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()
