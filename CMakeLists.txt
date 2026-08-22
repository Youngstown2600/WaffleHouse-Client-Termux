cmake_minimum_required(VERSION 3.20)

project(WaffleHouseClientTermux VERSION 3.2.0 LANGUAGES CXX)

set(APP_DISPLAY_NAME "WaffleHouse-Client" CACHE STRING "Application display name")
set(APP_ID "WaffleHouseClient" CACHE STRING "Application settings/config identifier")
set(APP_EXECUTABLE "wafflehouse-client" CACHE STRING "Executable name")
set(APP_VERSION_STRING "3.2-Termux" CACHE STRING "Application version string")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

find_package(Threads REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(NCURSESW REQUIRED IMPORTED_TARGET ncursesw)

# WaffleHouse-Client 3.2-Termux keeps the same PJSUA2 2.17 SIP core as the
# desktop source baseline. The Termux builder supplies a locally managed PJSIP
# configured against Termux PortAudio, whose package carries an OpenSL ES
# backend for Android.
pkg_check_modules(PJPROJECT REQUIRED libpjproject)
if(NOT PJPROJECT_VERSION VERSION_EQUAL "2.17")
    message(FATAL_ERROR "WaffleHouse-Client 3.2-Termux requires PJSIP 2.17; pkg-config reported ${PJPROJECT_VERSION}")
endif()
if(NOT PJPROJECT_STATIC_LDFLAGS)
    message(FATAL_ERROR "pkg-config did not return static PJSIP flags. Check: pkg-config --cflags --libs --static libpjproject")
endif()

include(GNUInstallDirs)

add_library(wafflehouse_sipcore
    src/sipcore/Profile.cpp
    src/sipcore/TextPool.cpp
    src/sipcore/Logger.cpp
    src/sipcore/SipTrace.cpp
    src/sipcore/CallSession.cpp
    src/sipcore/SipAccount.cpp
    src/sipcore/SipEngine.cpp
    src/sipcore/SipWireMonitor.cpp
    src/sipcore/RuntimePaths.cpp
)
target_include_directories(wafflehouse_sipcore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PJPROJECT_INCLUDE_DIRS}
)
target_compile_options(wafflehouse_sipcore PUBLIC ${PJPROJECT_CFLAGS_OTHER})
target_link_libraries(wafflehouse_sipcore PUBLIC ${PJPROJECT_STATIC_LDFLAGS} Threads::Threads)

add_executable(${APP_EXECUTABLE}
    src/main_termux.cpp
    src/mediacontroller.cpp
    src/backend.cpp
    src/ircbackend.cpp
    src/oscarprotocol.cpp
    src/oscarbackend.cpp
    src/telnetbackend.cpp
    src/ansiterminal.cpp
    src/bbsdirectory.cpp
    src/securechannel.cpp
    src/secureroom.cpp
    src/filetransfer.cpp
    src/directtransfer.cpp
    src/platforminfo.cpp
    src/notificationmanager.cpp
    src/useractivity.cpp
    src/terminalui.cpp
    src/sipcontroller.cpp
    src/sipbackend.cpp
)

target_include_directories(${APP_EXECUTABLE} PRIVATE src include)
target_compile_definitions(${APP_EXECUTABLE} PRIVATE
    APP_DISPLAY_NAME="${APP_DISPLAY_NAME}"
    APP_ID="${APP_ID}"
    APP_EXECUTABLE="${APP_EXECUTABLE}"
    APP_VERSION_STRING="${APP_VERSION_STRING}"
    WAFFLEHOUSE_TERMUX=1
    WAFFLEHOUSE_SOURCE_SOUND_DIR="${CMAKE_CURRENT_SOURCE_DIR}/sounds"
)

target_link_libraries(${APP_EXECUTABLE} PRIVATE
    wafflehouse_sipcore
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    PkgConfig::SODIUM
    PkgConfig::NCURSESW
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${APP_EXECUTABLE} PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(wafflehouse_sipcore PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS ${APP_EXECUTABLE}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY sounds/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/wafflehouse-client/sounds
    FILES_MATCHING PATTERN "*.wav"
)
