if(TARGET SmbProto)
    add_executable(test_smb test_smb.cpp)
    target_link_libraries(test_smb PUBLIC SmbProto)

    if(TARGET ServerServiceNasSmbAdapter)
        add_executable(test_smb_internal_client_smoke test_smb_internal_client_smoke.cpp)
        target_link_libraries(test_smb_internal_client_smoke PUBLIC ServerServiceNasSmbAdapter)
    endif()

    if(TARGET ServerServiceNasSmbAdapter AND TARGET ServerServiceSmb)
        add_executable(test_smb_nas_fixture test_smb_nas_fixture.cpp)
        target_link_libraries(test_smb_nas_fixture PUBLIC ServerServiceNasSmbAdapter ServerServiceSmb)
    endif()

    if(BUILD_TESTING)
        add_test(NAME smb COMMAND test_smb)
        if(TARGET test_smb_internal_client_smoke)
            add_test(NAME smb_internal_client_smoke COMMAND test_smb_internal_client_smoke)
            add_test(NAME smb_internal_client_smoke_basic COMMAND test_smb_internal_client_smoke --mode basic)
            add_test(NAME smb_internal_client_smoke_ioctl COMMAND test_smb_internal_client_smoke --mode ioctl)
        endif()
        if(WIN32)
            find_program(_smb_powershell NAMES pwsh powershell powershell.exe)
            if(NOT _smb_powershell)
                set(_smb_powershell "${CMAKE_COMMAND}" -E false)
            endif()
            set(_smb_smoke_cmd
                ${_smb_powershell} -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/smbclient_nas_smoke.ps1")
        else()
            set(_smb_smoke_cmd
                bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/smbclient_nas_smoke.sh")
        endif()

        add_test(NAME smbclient_nas_smoke COMMAND ${_smb_smoke_cmd})
        if(TARGET test_smb_nas_fixture)
            set_tests_properties(smbclient_nas_smoke PROPERTIES
                SKIP_RETURN_CODE 77
                ENVIRONMENT "YUAN_SMB_FIXTURE_BIN=$<TARGET_FILE:test_smb_nas_fixture>;YUAN_SMB_USE_FIXTURE=1;YUAN_SMB_PORT=15445")
        else()
            set_tests_properties(smbclient_nas_smoke PROPERTIES SKIP_RETURN_CODE 77)
        endif()

        add_test(NAME smbclient_nas_smoke_signing_required COMMAND ${_smb_smoke_cmd})
        if(TARGET test_smb_nas_fixture)
            set_tests_properties(smbclient_nas_smoke_signing_required PROPERTIES
                SKIP_RETURN_CODE 77
                ENVIRONMENT "YUAN_SMB_FIXTURE_BIN=$<TARGET_FILE:test_smb_nas_fixture>;YUAN_SMB_USE_FIXTURE=1;YUAN_SMB_PORT=15445;YUAN_SMB_REQUIRE_SIGNING=1;YUAN_SMBCLIENT_SIGNING=required")
        else()
            set_tests_properties(smbclient_nas_smoke_signing_required PROPERTIES SKIP_RETURN_CODE 77)
        endif()

        if(NOT WIN32)
            add_test(NAME smbclient_nas_cifs_live_smoke
                COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/nas_service_cifs_mount_smoke.sh")
            set_tests_properties(smbclient_nas_cifs_live_smoke PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endif()
endif()
