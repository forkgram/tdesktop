# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

function(telegram_add_apple_swift_runtime target_name)
    if (NOT APPLE)
        return()
    endif()

    target_link_options(${target_name}
    PRIVATE
        "-Wl,-rpath,/usr/lib/swift"
        "-Wl,-rpath,@executable_path/../Frameworks"
    )

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND mkdir -p $<TARGET_FILE_DIR:${target_name}>/../Frameworks
        COMMAND xcrun swift-stdlib-tool
            --copy
            --platform macosx
            --scan-executable $<TARGET_FILE:${target_name}>
            --destination $<TARGET_FILE_DIR:${target_name}>/../Frameworks
        VERBATIM
    )
endfunction()
