# Direct RayQuery shadow visibility and independent depth-aware reconstruction.
find_program(DX12_NATIVE_DXC NAMES dxc.exe dxc HINTS
    "$ENV{WindowsSdkVerBinPath}/x64"
    "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
if(DX12_NATIVE_DXC)
    set(_native_dir "${CMAKE_CURRENT_BINARY_DIR}/DX12NativeComposite")
    set(_native_shaders "")
    foreach(_shader PreparedShadowMask ReconstructShadowMask VisualizeShadowMask)
        set(_output "${_native_dir}/${_shader}.dxil")
        add_custom_command(
            OUTPUT "${_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_native_dir}"
            COMMAND "${DX12_NATIVE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
                    -Fo "${_output}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Composite/Shaders/${_shader}.hlsl6"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/DX12 Native Composite/Shaders/${_shader}.hlsl6"
            VERBATIM)
        list(APPEND _native_shaders "${_output}")
    endforeach()
    add_custom_target(DX12NativeCompositeDXIL DEPENDS ${_native_shaders})
    add_dependencies(${PROJECT_NAME} DX12NativeCompositeDXIL)
    register_feature_payload("DX12 Native Composite" FILES ${_native_shaders}
        DESTINATION "Shaders/DX12NativeComposite")
else()
    if(AIO_ZIP_TO_DIST OR AUTO_PLUGIN_DEPLOYMENT)
        message(FATAL_ERROR "DXC is required to compile DX12 Native Composite DXIL")
    endif()
    message(WARNING "DXC not found; native DXR shadowmask remains disabled")
endif()
