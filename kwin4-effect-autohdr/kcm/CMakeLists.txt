# SPDX-FileCopyrightText: 2026 Luu
# SPDX-License-Identifier: GPL-2.0-or-later

kcoreaddons_add_plugin(kwin4_effect_autohdr_config
    INSTALL_NAMESPACE "kwin/effects/configs"
    SOURCES
        autohdr_effect_kcm.cpp
        tone_curve_editor.cpp
        ../autohdr_config.cpp
        ../tone_curve.cpp
)

target_include_directories(kwin4_effect_autohdr_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(kwin4_effect_autohdr_config
    PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::DBus
    KF6::ConfigCore
    KF6::ConfigWidgets
    KF6::KCMUtils
    KF6::I18n
    KF6::CoreAddons
)

target_compile_definitions(kwin4_effect_autohdr_config PRIVATE
    -DTRANSLATION_DOMAIN=\"kwin4_effect_autohdr\"
)
