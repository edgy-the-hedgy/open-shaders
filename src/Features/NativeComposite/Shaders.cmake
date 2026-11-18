if(DX12_NATIVE_SCENE_DXC)
    set(NATIVE_COMPOSITE_DXIL_DIR "${CMAKE_CURRENT_BINARY_DIR}/NativeComposite")
    set(NATIVE_COMPOSITE_DXIL "${NATIVE_COMPOSITE_DXIL_DIR}/AddLighting.dxil")
    set(NATIVE_COMPOSITE_SHADOW_DXIL
        "${NATIVE_COMPOSITE_DXIL_DIR}/ShadowLayer.dxil"
    )
    set(NATIVE_COMPOSITE_ORIGIN_DXIL
        "${NATIVE_COMPOSITE_DXIL_DIR}/OriginPreview.dxil"
    )
    set(NATIVE_COMPOSITE_BLEND_DXIL
        "${NATIVE_COMPOSITE_DXIL_DIR}/BlendSources.dxil"
    )
    set(NATIVE_COMPOSITE_DEPTH_DXIL
        "${NATIVE_COMPOSITE_DXIL_DIR}/NativeDepthGuide.dxil"
    )
    set(NATIVE_COMPOSITE_MOTION_DXIL
        "${NATIVE_COMPOSITE_DXIL_DIR}/NativeMotionGuide.dxil"
    )
    set(NATIVE_COMPOSITE_MASK_DXIL
        "${NATIVE_COMPOSITE_DXIL_DIR}/NativeOpaqueMasks.dxil"
    )
    set(NATIVE_COMPOSITE_COMPARE_DXIL
        "${NATIVE_COMPOSITE_DXIL_DIR}/CompareGuides1.dxil"
        "${NATIVE_COMPOSITE_DXIL_DIR}/CompareGuides2.dxil"
        "${NATIVE_COMPOSITE_DXIL_DIR}/CompareGuides4.dxil"
    )
    add_custom_command(
        OUTPUT "${NATIVE_COMPOSITE_DXIL}" "${NATIVE_COMPOSITE_SHADOW_DXIL}" "${NATIVE_COMPOSITE_ORIGIN_DXIL}" "${NATIVE_COMPOSITE_BLEND_DXIL}" "${NATIVE_COMPOSITE_DEPTH_DXIL}" "${NATIVE_COMPOSITE_MOTION_DXIL}" "${NATIVE_COMPOSITE_MASK_DXIL}" ${NATIVE_COMPOSITE_COMPARE_DXIL}
        COMMAND
            ${CMAKE_COMMAND} -E make_directory "${NATIVE_COMPOSITE_DXIL_DIR}"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -Fo "${NATIVE_COMPOSITE_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/AddLighting.hlsl6"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -Fo "${NATIVE_COMPOSITE_SHADOW_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/ShadowLayer.hlsl6"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -Fo "${NATIVE_COMPOSITE_ORIGIN_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/OriginPreview.hlsl6"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -Fo "${NATIVE_COMPOSITE_BLEND_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/BlendSources.hlsl6"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -Fo "${NATIVE_COMPOSITE_DEPTH_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativeDepthGuide.hlsl6"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -Fo "${NATIVE_COMPOSITE_MOTION_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativeMotionGuide.hlsl6"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis
            -Fo "${NATIVE_COMPOSITE_MASK_DXIL}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativeOpaqueMasks.hlsl6"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis -D GUIDE_COMPONENTS=1
            -Fo "${NATIVE_COMPOSITE_DXIL_DIR}/CompareGuides1.dxil"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/CompareGuides.hlsl6"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis -D GUIDE_COMPONENTS=2
            -Fo "${NATIVE_COMPOSITE_DXIL_DIR}/CompareGuides2.dxil"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/CompareGuides.hlsl6"
        COMMAND
            "${DX12_NATIVE_SCENE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -Gis -D GUIDE_COMPONENTS=4
            -Fo "${NATIVE_COMPOSITE_DXIL_DIR}/CompareGuides4.dxil"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/CompareGuides.hlsl6"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/AddLighting.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/ShadowLayer.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/OriginPreview.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/BlendSources.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativeDepthGuide.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativeMotionGuide.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativeOpaqueMasks.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/CompareGuides.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/GuideDecision.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativePBRMaterial.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/NativePBRBRDF.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Native Composite/Shaders/NativeComposite/ValidityPolicy.hlsli"
        VERBATIM
    )
    add_custom_target(
        NativeCompositeDXIL
        DEPENDS "${NATIVE_COMPOSITE_DXIL}" "${NATIVE_COMPOSITE_SHADOW_DXIL}" "${NATIVE_COMPOSITE_ORIGIN_DXIL}" "${NATIVE_COMPOSITE_BLEND_DXIL}" "${NATIVE_COMPOSITE_DEPTH_DXIL}" "${NATIVE_COMPOSITE_MOTION_DXIL}" "${NATIVE_COMPOSITE_MASK_DXIL}" ${NATIVE_COMPOSITE_COMPARE_DXIL}
    )
    add_dependencies(${PROJECT_NAME} NativeCompositeDXIL)
    register_feature_payload(NativeComposite FILES "${NATIVE_COMPOSITE_DXIL}" "${NATIVE_COMPOSITE_SHADOW_DXIL}" "${NATIVE_COMPOSITE_ORIGIN_DXIL}" "${NATIVE_COMPOSITE_BLEND_DXIL}" "${NATIVE_COMPOSITE_DEPTH_DXIL}" "${NATIVE_COMPOSITE_MOTION_DXIL}" "${NATIVE_COMPOSITE_MASK_DXIL}" ${NATIVE_COMPOSITE_COMPARE_DXIL} DESTINATION "Shaders/NativeComposite")
    install(
        FILES "${NATIVE_COMPOSITE_DXIL}" "${NATIVE_COMPOSITE_SHADOW_DXIL}" "${NATIVE_COMPOSITE_ORIGIN_DXIL}" "${NATIVE_COMPOSITE_BLEND_DXIL}" "${NATIVE_COMPOSITE_DEPTH_DXIL}" "${NATIVE_COMPOSITE_MOTION_DXIL}" "${NATIVE_COMPOSITE_MASK_DXIL}" ${NATIVE_COMPOSITE_COMPARE_DXIL}
        DESTINATION "Shaders/NativeComposite"
        COMPONENT Shaders
    )
endif()
