# DX12 Resources is a self-contained, geometry-and-light-only .NET companion.
# Optional reference tool; runtime-only acquisition does not launch it.
if(NOT WIN32)
    return()
endif()

option(
    BUILD_DX12_RESOURCES_COMPANION
    "Build and include the DX12 Resources companion executable in Windows AIO packages"
    OFF
)
if(NOT BUILD_DX12_RESOURCES_COMPANION)
    message(
        WARNING
        "DX12 Resources companion disabled; AIO will not contain its executable"
    )
    return()
endif()

set(DX12_RESOURCES_PREBUILT_EXE "" CACHE FILEPATH "Optional prebuilt companion executable")
set(DX12_RESOURCES_PREBUILT_SHA256 "" CACHE STRING "Expected SHA256 for the prebuilt companion")
find_program(DX12_RESOURCES_DOTNET NAMES dotnet.exe dotnet)
set(_dx12_companion_project
    "${CMAKE_SOURCE_DIR}/tools/dx12-resources-prep/DX12ResourcesPrep.csproj"
)
set(_dx12_companion_output
    "${CMAKE_CURRENT_BINARY_DIR}/dx12-resources-prep/publish"
)
set(_dx12_companion_exe "${_dx12_companion_output}/DX12ResourcesPrep.exe")
set(_dx12_companion_aio_dir
    "${AIO_DIR}/SKSE/Plugins/CommunityShaders/DX12Resources/bin"
)
set(DX12_RESOURCES_AIO_EXE "${_dx12_companion_aio_dir}/DX12ResourcesPrep.exe")

file(
    GLOB _dx12_companion_sources
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/dx12-resources-prep/*.cs"
)

if(DX12_RESOURCES_PREBUILT_EXE)
    if(NOT EXISTS "${DX12_RESOURCES_PREBUILT_EXE}" OR NOT DX12_RESOURCES_PREBUILT_SHA256)
        message(FATAL_ERROR "A prebuilt companion requires an existing executable and a SHA256")
    endif()
    file(SHA256 "${DX12_RESOURCES_PREBUILT_EXE}" _prebuilt_hash)
    string(TOLOWER "${DX12_RESOURCES_PREBUILT_SHA256}" _expected_hash)
    if(NOT _prebuilt_hash STREQUAL _expected_hash)
        message(FATAL_ERROR "Prebuilt DX12 Resources companion SHA256 mismatch")
    endif()
    add_custom_command(
        OUTPUT "${_dx12_companion_exe}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dx12_companion_output}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DX12_RESOURCES_PREBUILT_EXE}" "${_dx12_companion_exe}"
        DEPENDS "${DX12_RESOURCES_PREBUILT_EXE}"
        VERBATIM
    )
elseif(DX12_RESOURCES_DOTNET)
    add_custom_command(
        OUTPUT "${_dx12_companion_exe}"
        COMMAND
            "${DX12_RESOURCES_DOTNET}" publish "${_dx12_companion_project}"
            --configuration Release --runtime win-x64 --self-contained true
            -p:PublishSingleFile=true -p:PublishReadyToRun=false --output
            "${_dx12_companion_output}"
        DEPENDS "${_dx12_companion_project}" ${_dx12_companion_sources}
        COMMENT "Publishing self-contained DX12 Resources companion (.NET 10)"
        VERBATIM
    )
else()
    # Preserve DLL-only builds without dotnet. An AIO build MUST fail rather
    # than silently shipping without the executable required by runtime.
    message(
        WARNING
        "dotnet was not found; DX12 AIO packaging requires the .NET 10 SDK"
    )
    add_custom_command(
        OUTPUT "${_dx12_companion_exe}"
        COMMAND
            ${CMAKE_COMMAND} -E echo
            "DX12 Resources companion requires the .NET 10 SDK (dotnet publish)"
        COMMAND ${CMAKE_COMMAND} -E false
        DEPENDS "${_dx12_companion_project}" ${_dx12_companion_sources}
        COMMENT "Missing .NET 10 SDK for DX12 Resources AIO payload"
        VERBATIM
    )
endif()

add_custom_target(
    BUILD_DX12_RESOURCES_COMPANION
    DEPENDS "${_dx12_companion_exe}"
)

# The manual AIO runs cmake --install after wiping AIO; reinstall the binary.
install(
    FILES "${_dx12_companion_exe}"
    DESTINATION "SKSE/Plugins/CommunityShaders/DX12Resources/bin"
    COMPONENT Shaders
)

# Stage *only* the single-file executable. It owns no persistent texture,
# material, atlas or geometry cache and does not need publish diagnostics/PDBs.
add_custom_command(
    OUTPUT "${DX12_RESOURCES_AIO_EXE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_dx12_companion_aio_dir}"
    COMMAND
        ${CMAKE_COMMAND} -E copy_if_different "${_dx12_companion_exe}"
        "${DX12_RESOURCES_AIO_EXE}"
    DEPENDS BUILD_DX12_RESOURCES_COMPANION "${_dx12_companion_exe}"
    COMMENT "Staging DX12 Resources companion into AIO SKSE payload"
    VERBATIM
)
add_custom_target(
    STAGE_DX12_RESOURCES_COMPANION
    DEPENDS "${DX12_RESOURCES_AIO_EXE}"
)
if(TARGET PREPARE_AIO)
    # The AIO source directory is cleaned/rebuilt by PREPARE_AIO. Never race
    # its writes or the branch-change reset with companion staging.
    add_dependencies(STAGE_DX12_RESOURCES_COMPANION PREPARE_AIO)
endif()

if(AUTO_PLUGIN_DEPLOYMENT)
    add_custom_target(
        DEPLOY_DX12_RESOURCES_COMPANION
        DEPENDS STAGE_DX12_RESOURCES_COMPANION
    )
    foreach(_dx12_deploy_root $ENV{CommunityShadersOutputDir})
        set(_dx12_deploy_dir
            "${_dx12_deploy_root}/SKSE/Plugins/CommunityShaders/DX12Resources/bin"
        )
        add_custom_command(
            TARGET DEPLOY_DX12_RESOURCES_COMPANION
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_dx12_deploy_dir}"
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different
                "${DX12_RESOURCES_AIO_EXE}"
                "${_dx12_deploy_dir}/DX12ResourcesPrep.exe"
            VERBATIM
        )
    endforeach()
    if(TARGET DEPLOY_ALL)
        add_dependencies(DEPLOY_ALL DEPLOY_DX12_RESOURCES_COMPANION)
    endif()
endif()
