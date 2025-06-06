# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_library(web_service STATIC
    announce_room_json.cpp
    announce_room_json.h
    precompiled_headers.h
    telemetry_json.cpp
    telemetry_json.h
    verify_login.cpp
    verify_login.h
    verify_user_jwt.cpp
    verify_user_jwt.h
    web_backend.cpp
    web_backend.h
    web_result.h
)

create_target_directory_groups(web_service)
target_link_libraries(web_service PRIVATE common network nlohmann_json::nlohmann_json httplib::httplib cpp-jwt::cpp-jwt)

if (SUMI_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(web_service PRIVATE precompiled_headers.h)
endif()
