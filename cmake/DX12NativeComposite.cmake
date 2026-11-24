# Native Composite uses the same DXC compiler and shader delivery as Native Scene.
if(NATIVE_SHADOW_DXC)
    set(_native_composite_source
        "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/ShadowLayer.hlsl6"
    )
    set(_native_surface "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativeSurface.hlsli")
    set(_native_composite_dir "${CMAKE_CURRENT_BINARY_DIR}/NativeComposite")
    set(_native_composite_dxil "${_native_composite_dir}/ShadowLayer.dxil")
    add_custom_command(
        OUTPUT "${_native_composite_dxil}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_native_composite_dir}"
        COMMAND
            "${NATIVE_SHADOW_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis -Fo
            "${_native_composite_dxil}" "${_native_composite_source}"
        DEPENDS "${_native_composite_source}" "${_native_surface}"
        VERBATIM
    )
    set(_native_pbr_probe_source "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativePBRProbe.hlsl6")
    set(_native_material_alpha "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativeMaterialAlpha.hlsli")
    set(_native_pbr_probe_dxil "${_native_composite_dir}/NativePBRProbe.dxil")
    add_custom_command(
        OUTPUT "${_native_pbr_probe_dxil}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_native_composite_dir}"
        COMMAND "${NATIVE_SHADOW_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis -Fo
            "${_native_pbr_probe_dxil}" "${_native_pbr_probe_source}"
        DEPENDS "${_native_pbr_probe_source}" "${_native_material_alpha}" "${_native_surface}"
        VERBATIM
    )
    set(_native_pbr_screen_dxil "${_native_composite_dir}/NativePBRScreenProbe.dxil")
    add_custom_command(
        OUTPUT "${_native_pbr_screen_dxil}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_native_composite_dir}"
        COMMAND "${NATIVE_SHADOW_DXC}" -T cs_6_5 -E screenMain -HV 2021 -O3 -Ges -Gis -Fo
            "${_native_pbr_screen_dxil}" "${_native_pbr_probe_source}"
        DEPENDS "${_native_pbr_probe_source}" "${_native_material_alpha}" "${_native_surface}"
        VERBATIM
    )
    set(_native_direct_source "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/DirectLighting.hlsl6")
    set(_native_direct_dxil "${_native_composite_dir}/DirectLighting.dxil")
    add_custom_command(
        OUTPUT "${_native_direct_dxil}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_native_composite_dir}"
        COMMAND "${NATIVE_SHADOW_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis -Fo
            "${_native_direct_dxil}" "${_native_direct_source}"
        DEPENDS "${_native_direct_source}" "${_native_material_alpha}" "${_native_surface}"
        VERBATIM
    )
    set(_native_common "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders")
    set(_native_indirect_dir "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite")
    set(_native_indirect_deps
        "${_native_common}/Common/Math.hlsli"
        "${_native_common}/Common/BRDF.hlsli"
        "${_native_common}/Common/Shading.hlsli"
        "${_native_common}/Common/Spherical Harmonics/SphericalHarmonics.hlsli"
        "${_native_indirect_dir}/NativeIndirectBRDF.hlsli"
        "${_native_surface}"
    )
    set(_native_indirect_outputs)
    set(_native_indirect_aio_outputs)
    foreach(_shader NativeDiffuseSH NativeIndirectLighting)
        set(_source "${_native_indirect_dir}/${_shader}.hlsl6")
        set(_output "${_native_composite_dir}/${_shader}.dxil")
        add_custom_command(
            OUTPUT "${_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_native_composite_dir}"
            COMMAND "${NATIVE_SHADOW_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
                -I "${_native_common}" -Fo "${_output}" "${_source}"
            DEPENDS "${_source}" ${_native_indirect_deps}
            VERBATIM
        )
        list(APPEND _native_indirect_outputs "${_output}")
        if(DEFINED AIO_DIR)
            set(_aio_output "${AIO_DIR}/Shaders/NativeComposite/${_shader}.dxil")
            add_custom_command(
                OUTPUT "${_aio_output}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${AIO_DIR}/Shaders/NativeComposite"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_output}" "${_aio_output}"
                DEPENDS "${_output}"
                VERBATIM
            )
            list(APPEND _native_indirect_aio_outputs "${_aio_output}")
        endif()
    endforeach()
    add_custom_target(
        DX12NativeCompositeDXIL
        DEPENDS "${_native_composite_dxil}" "${_native_pbr_probe_dxil}" "${_native_pbr_screen_dxil}" "${_native_direct_dxil}" ${_native_indirect_outputs}
    )
    add_dependencies(${PROJECT_NAME} DX12NativeCompositeDXIL)
    if(DEFINED AIO_DIR)
        set(_native_composite_aio_dxil
            "${AIO_DIR}/Shaders/NativeComposite/ShadowLayer.dxil"
        )
        add_custom_command(
            OUTPUT "${_native_composite_aio_dxil}"
            COMMAND
                ${CMAKE_COMMAND} -E make_directory
                "${AIO_DIR}/Shaders/NativeComposite"
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different
                "${_native_composite_dxil}" "${_native_composite_aio_dxil}"
            DEPENDS "${_native_composite_dxil}"
            COMMENT "Staging DXIL ShadowLayer.dxil into the AIO shader tree"
            VERBATIM
        )
        set(_native_pbr_probe_aio_dxil "${AIO_DIR}/Shaders/NativeComposite/NativePBRProbe.dxil")
        add_custom_command(
            OUTPUT "${_native_pbr_probe_aio_dxil}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${AIO_DIR}/Shaders/NativeComposite"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_native_pbr_probe_dxil}" "${_native_pbr_probe_aio_dxil}"
            DEPENDS "${_native_pbr_probe_dxil}"
            VERBATIM
        )
        set(_native_pbr_screen_aio_dxil "${AIO_DIR}/Shaders/NativeComposite/NativePBRScreenProbe.dxil")
        add_custom_command(
            OUTPUT "${_native_pbr_screen_aio_dxil}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${AIO_DIR}/Shaders/NativeComposite"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_native_pbr_screen_dxil}" "${_native_pbr_screen_aio_dxil}"
            DEPENDS "${_native_pbr_screen_dxil}"
            VERBATIM
        )
        set(_native_direct_aio_dxil "${AIO_DIR}/Shaders/NativeComposite/DirectLighting.dxil")
        add_custom_command(
            OUTPUT "${_native_direct_aio_dxil}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${AIO_DIR}/Shaders/NativeComposite"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_native_direct_dxil}" "${_native_direct_aio_dxil}"
            DEPENDS "${_native_direct_dxil}"
            VERBATIM
        )
        add_custom_target(
            DX12NativeCompositeAIO
            DEPENDS "${_native_composite_aio_dxil}" "${_native_pbr_probe_aio_dxil}" "${_native_pbr_screen_aio_dxil}" "${_native_direct_aio_dxil}" ${_native_indirect_aio_outputs}
        )
        add_dependencies(DX12NativeCompositeAIO DX12NativeCompositeDXIL)
        if(TARGET prepare_shaders)
            add_dependencies(DX12NativeCompositeAIO prepare_shaders)
        endif()
        if(TARGET PREPARE_AIO)
            add_dependencies(DX12NativeCompositeAIO PREPARE_AIO)
        endif()
    endif()
    install(
        FILES "${_native_composite_dxil}" "${_native_pbr_probe_dxil}" "${_native_pbr_screen_dxil}" "${_native_direct_dxil}" ${_native_indirect_outputs}
        DESTINATION "Shaders/NativeComposite"
        COMPONENT Shaders
    )
endif()
