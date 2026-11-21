# Native Composite currently produces only an independent DXR shadowmask.
# The retired whole-raster-color multiplier is not a correct per-light compositor.
find_program(
    DX12_NATIVE_DXC
    NAMES dxc.exe dxc
    HINTS
        "$ENV{WindowsSdkVerBinPath}/x64"
        "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
)

if(DX12_NATIVE_DXC)
    set(_dx12_native_dir "${CMAKE_CURRENT_BINARY_DIR}/DX12NativeComposite")
    set(_dx12_mask "${_dx12_native_dir}/PreparedShadowMask.dxil")
    add_custom_command(
        OUTPUT "${_dx12_mask}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dx12_native_dir}"
        COMMAND
            "${DX12_NATIVE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis -Fo
            "${_dx12_mask}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Composite/Shaders/PreparedShadowMask.hlsl6"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Composite/Shaders/PreparedShadowMask.hlsl6"
        VERBATIM
    )
    set(_dx12_visual "${_dx12_native_dir}/VisualizeShadowMask.dxil")
    add_custom_command(
        OUTPUT "${_dx12_visual}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dx12_native_dir}"
        COMMAND
            "${DX12_NATIVE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis -Fo
            "${_dx12_visual}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Composite/Shaders/VisualizeShadowMask.hlsl6"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Composite/Shaders/VisualizeShadowMask.hlsl6"
        VERBATIM
    )
    add_custom_target(DX12NativeCompositeDXIL DEPENDS "${_dx12_mask}" "${_dx12_visual}")
    add_dependencies(${PROJECT_NAME} DX12NativeCompositeDXIL)
    register_feature_payload("DX12 Native Composite"
        FILES "${_dx12_mask}" "${_dx12_visual}"
        DESTINATION "Shaders/DX12NativeComposite"
    )
else()
    # This is a required binary for DX12 shadowmask initialization, and not
    # shipping it produces a permanent in-game pipeline failure.
    if(AIO_ZIP_TO_DIST OR AUTO_PLUGIN_DEPLOYMENT)
        message(FATAL_ERROR
            "DXC (dxc.exe) is required to compile and package DX12 Native Composite. "
            "Install the Windows SDK DirectX Shader Compiler and re-run CMake configuration.")
    endif()
    message(WARNING "DXC not found; native DXR shadowmask remains disabled")
endif()
