cmake_minimum_required(VERSION 3.16)

project(kwin-effect-autohdr VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

set(QT_DEFAULT_MAJOR_VERSION 6)

find_package(Qt6 REQUIRED COMPONENTS Core Gui DBus Widgets)
find_package(KWin REQUIRED)
find_package(KF6 REQUIRED COMPONENTS Config ConfigWidgets KCMUtils CoreAddons GlobalAccel I18n)

include(KDEInstallDirs)
include(KDECMakeSettings)

set(CMAKE_AUTOMOC ON)

kcoreaddons_add_plugin(kwin4_effect_autohdr
    SOURCES
        autohdr_effect.cpp
        autohdr_config.cpp
        tone_curve.cpp
        tone_curve_presets.cpp
        tone_curve_user_presets.cpp
    INSTALL_NAMESPACE "kwin/effects/plugins"
)

target_link_libraries(kwin4_effect_autohdr
    PRIVATE
    KWin::kwin
    Qt6::Core
    Qt6::Gui
    Qt6::DBus
    KF6::CoreAddons
    KF6::ConfigCore
    KF6::GlobalAccel
    KF6::I18n
)

install(FILES autohdr.frag autohdr_color.glsl
    DESTINATION ${KDE_INSTALL_DATADIR}/kwin/effects/autohdr)
install(PROGRAMS plasma-autohdr-calibrate
    DESTINATION ${KDE_INSTALL_DATADIR}/kwin/effects/autohdr)
install(FILES kwin4_effect_autohdr.shortcuts
    DESTINATION ${KDE_INSTALL_DATADIR}/kglobalaccel)

add_subdirectory(kcm)
