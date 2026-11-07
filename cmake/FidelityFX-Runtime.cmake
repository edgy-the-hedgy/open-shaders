set(FFX_RUNTIME_SDK_COMMIT "7539ae8b6568ad29ed78a753404351daf3b5cca7")
set(FFX_RUNTIME_BASE_URL
    "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/${FFX_RUNTIME_SDK_COMMIT}/Kits/FidelityFX/signedbin"
)
set(FFX_RUNTIME_FEATURE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/ffx-runtime")
set(FFX_RUNTIME_SHADER_ROOT "${FFX_RUNTIME_FEATURE_ROOT}/Shaders")
set(FFX_RUNTIME_RELATIVE_DIRECTORY "Shaders/Upscaling/FidelityFX")
set(FFX_RUNTIME_DIRECTORY
    "${FFX_RUNTIME_FEATURE_ROOT}/${FFX_RUNTIME_RELATIVE_DIRECTORY}"
)
file(MAKE_DIRECTORY "${FFX_RUNTIME_DIRECTORY}")

function(download_ffx_runtime _filename _sha256)
    set(_destination "${FFX_RUNTIME_DIRECTORY}/${_filename}")
    if(SKIP_RUNTIME_DOWNLOADS)
        # Registering the expected path makes a package install fail on the
        # absent DLL instead of shipping without it.
        set(FFX_RUNTIME_PAYLOAD_FILES
            ${FFX_RUNTIME_PAYLOAD_FILES}
            "${_destination}"
            PARENT_SCOPE
        )
        return()
    endif()
    # AMD distributes the pinned denoiser alongside the other FFX runtimes.
    # Its upstream Git blob ID provides an exact byte-for-byte content check
    # when a SHA256 has not yet been recorded locally.
    if(_sha256 MATCHES "^GITBLOB:")
        string(SUBSTRING "${_sha256}" 8 -1 _expected_git_blob)
        string(LENGTH "${_expected_git_blob}" _git_blob_length)
        if(NOT _git_blob_length EQUAL 40)
            message(FATAL_ERROR "Invalid pinned FidelityFX Git blob ID for ${_filename}")
        endif()
        file(DOWNLOAD "${FFX_RUNTIME_BASE_URL}/${_filename}" "${_destination}"
            STATUS _download_status TLS_VERIFY ON TIMEOUT 120 INACTIVITY_TIMEOUT 20)
        list(GET _download_status 0 _download_code)
        if(_download_code EQUAL 0)
            if(NOT GIT_FOUND)
                message(FATAL_ERROR "Git is required to validate pinned FidelityFX denoiser runtime")
            endif()
            execute_process(COMMAND "${GIT_EXECUTABLE}" hash-object "${_destination}"
                OUTPUT_VARIABLE _download_git_blob OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _hash_result)
            if(NOT _hash_result EQUAL 0 OR NOT _download_git_blob STREQUAL _expected_git_blob)
                file(REMOVE "${_destination}")
                message(FATAL_ERROR "Pinned FidelityFX runtime blob verification failed for ${_filename}")
            endif()
        endif()
    else()
        file(DOWNLOAD "${FFX_RUNTIME_BASE_URL}/${_filename}" "${_destination}"
            EXPECTED_HASH "SHA256=${_sha256}"
            STATUS _download_status TLS_VERIFY ON TIMEOUT 120 INACTIVITY_TIMEOUT 20)
    endif()
    list(GET _download_status 0 _status_code)
    list(GET _download_status 1 _status_message)
    if(NOT _status_code EQUAL 0)
        file(REMOVE "${_destination}")
        message(
            FATAL_ERROR
            "Failed to download ${_filename}: ${_status_message}"
        )
    endif()
    set(FFX_RUNTIME_PAYLOAD_FILES
        ${FFX_RUNTIME_PAYLOAD_FILES}
        "${_destination}"
        PARENT_SCOPE
    )
endfunction()

set(FFX_RUNTIME_PAYLOAD_FILES "")
download_ffx_runtime(
    amd_fidelityfx_framegeneration_dx12.dll
    E6843252F58B0341A0D80F6A20BC0D840643EE95BC48C22CAC08745344577BB6
)
download_ffx_runtime(
    amd_fidelityfx_loader_dx12.dll
    E2191D3C0828BE45EB8AFFD4E3EC0378F2293ADC345B772A7170EDCF3B021ECB
)
download_ffx_runtime(
    amd_fidelityfx_upscaler_dx12.dll
    AAD4FCCCCB29084A655F9A4F9F613BD22C8C03FB4A0F8840148CC6543B4CCACE
)
# Exact Git object identity from the FidelityFX-SDK revision above.
download_ffx_runtime(
    amd_fidelityfx_denoiser_dx12.dll
    GITBLOB:44b7655a740017a087c745966d98a6a834e59f12
)

split_runtime_payload(FFX_RUNTIME)

register_feature_payload(
    Upscaling
    FILES ${FFX_RUNTIME_PAYLOAD_FILES}
    DESTINATION "${FFX_RUNTIME_RELATIVE_DIRECTORY}"
)
