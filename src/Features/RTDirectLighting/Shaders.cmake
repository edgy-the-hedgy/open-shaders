find_program(REDSTONE_DXC NAMES dxc.exe dxc
    HINTS "$ENV{WindowsSdkVerBinPath}/x64"
          "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64"
          "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
set(REDSTONE_DXIL_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/RTDirectLighting")
set(REDSTONE_DXIL_FILES "")
set(REDSTONE_DXIL_CORE_COMMANDS "")
if(REDSTONE_DXC)
    set(_redstone_directional "${REDSTONE_DXIL_DIRECTORY}/directional.dxil")
    add_custom_command(OUTPUT "${_redstone_directional}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
        COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges
            -I "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/features/Light Limit Fix/Shaders"
            -Fo "${_redstone_directional}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/directional.hlsl6"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/directional.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/materials.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/directLighting.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/indirectLighting.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/directContract.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Light Limit Fix/Shaders/LightLimitFix/Attenuation.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/Light Limit Fix/Shaders/LightLimitFix/Common.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/Game.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/PointLightFlags.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/ReceiverRoom.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/PBRMath.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/LightingCommon.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/BRDF.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/Math.hlsli"
        VERBATIM)
    set(_shadow_temporal "${REDSTONE_DXIL_DIRECTORY}/shadowTemporal.dxil")
    add_custom_command(OUTPUT "${_shadow_temporal}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
        COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges
            -Fo "${_shadow_temporal}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/shadowTemporal.hlsl6"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/shadowTemporal.hlsl6"
        VERBATIM)
    set(_hybrid_source "${REDSTONE_DXIL_DIRECTORY}/hybridShadowSource.dxil")
    add_custom_command(OUTPUT "${_hybrid_source}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
        COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges
            -Fo "${_hybrid_source}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/hybridShadowSource.hlsl6"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/hybridShadowSource.hlsl6"
        VERBATIM)
    set(_rt_hybrid_0 "${REDSTONE_DXIL_DIRECTORY}/hybridGI.dxil")
    add_custom_command(OUTPUT "${_rt_hybrid_0}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
        COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -D DX12_HYBRID_GI=1
            -I "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders"
            -Fo "${_rt_hybrid_0}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/gi.cs.hlsl"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/gi.cs.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/Sampling.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/SpecularWeights.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/common.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/NativeGICompat.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/NativeGBuffer.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/OcclusionSettings.hlsli"
        VERBATIM)
    set(_rt_hybrid_1 "${REDSTONE_DXIL_DIRECTORY}/hybridAO.dxil")
    add_custom_command(OUTPUT "${_rt_hybrid_1}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
        COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges -D DX12_HYBRID_GI=1
            -I "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders"
            -Fo "${_rt_hybrid_1}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/occlusion.cs.hlsl"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/occlusion.cs.hlsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/Sampling.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/SpecularWeights.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/common.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/NativeGICompat.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/NativeGBuffer.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/OcclusionSettings.hlsli"
        VERBATIM)
    set(_rt_hybrid_eligibility "${REDSTONE_DXIL_DIRECTORY}/hybridEligibility.dxil")
    add_custom_command(OUTPUT "${_rt_hybrid_eligibility}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
        COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges
            -Fo "${_rt_hybrid_eligibility}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/hybridEligibility.hlsl6"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/hybridEligibility.hlsl6"
        VERBATIM)
    set(_rt_hybrid_compose "${REDSTONE_DXIL_DIRECTORY}/nativeHybridCompose.dxil")
    add_custom_command(OUTPUT "${_rt_hybrid_compose}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
        COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges
            -Fo "${_rt_hybrid_compose}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/nativeHybridCompose.hlsl6"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/nativeHybridCompose.hlsl6"
        VERBATIM)
    set(_rt_hybrid_history "${REDSTONE_DXIL_DIRECTORY}/nativeHybridRRHistory.dxil")
    add_custom_command(OUTPUT "${_rt_hybrid_history}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
        COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges
            -Fo "${_rt_hybrid_history}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/nativeHybridRRHistory.hlsl6"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/nativeHybridRRHistory.hlsl6"
        VERBATIM)
    set(_rt_hybrid_guides "${REDSTONE_DXIL_DIRECTORY}/nativeHybridRRGuides.dxil")
    add_custom_command(OUTPUT "${_rt_hybrid_guides}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
        COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges
            -I "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders"
            -Fo "${_rt_hybrid_guides}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/nativeHybridRRGuides.hlsl6"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/nativeHybridRRGuides.hlsl6"
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/FrameBuffer.hlsli"
        VERBATIM)
    # Optional post-RR passes: compiled and packaged now, but not dispatched
    # until native DX12 RR output and independent histories are wired.
    set(_rt_hybrid_reconstruct "${REDSTONE_DXIL_DIRECTORY}/hybridPostRRReconstruct.dxil")
    add_custom_command(OUTPUT "${_rt_hybrid_reconstruct}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
        COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges
            -Fo "${_rt_hybrid_reconstruct}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/hybridPostRRReconstruct.hlsl6"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/hybridPostRRReconstruct.hlsl6"
        VERBATIM)
    set(_rt_native_reconstruct "${REDSTONE_DXIL_DIRECTORY}/hybridNativeReconstruct.dxil")
    add_custom_command(OUTPUT "${_rt_native_reconstruct}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
        COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges
            -Fo "${_rt_native_reconstruct}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/hybridNativeReconstruct.hlsl6"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/hybridNativeReconstruct.hlsl6"
    )
    set(_rt_hybrid_blur "${REDSTONE_DXIL_DIRECTORY}/hybridPostRRBlur.dxil")
    add_custom_command(OUTPUT "${_rt_hybrid_blur}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
        COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges
            -Fo "${_rt_hybrid_blur}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/hybridPostRRBlur.hlsl6"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/hybridPostRRBlur.hlsl6"
        VERBATIM)
    set(_rt_advanced_pack "${REDSTONE_DXIL_DIRECTORY}/nativeAdvancedPack.dxil")
    add_custom_command(OUTPUT "${_rt_advanced_pack}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
        COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2021 -O3 -Ges
            -Fo "${_rt_advanced_pack}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/nativeAdvancedPack.hlsl6"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/nativeAdvancedPack.hlsl6"
        VERBATIM)
    # Deliberately opt-in until DX12 native estimator resource bindings exist:
    # CMake --build <build> --target RTComposerHybridDXIL compiles the exact
    # RedstoneGI estimator and AO shader for Shader Model 6.5.
    add_custom_target(RTComposerHybridDXIL DEPENDS "${_rt_hybrid_0}" "${_rt_hybrid_1}" "${_rt_hybrid_eligibility}" "${_rt_hybrid_compose}" "${_rt_hybrid_guides}" "${_rt_hybrid_history}" "${_rt_hybrid_reconstruct}" "${_rt_hybrid_blur}" "${_rt_native_reconstruct}" "${_rt_advanced_pack}")
    add_custom_target(RTDirectLightingDXIL DEPENDS
        "${_redstone_directional}" "${_shadow_temporal}" "${_hybrid_source}" "${_rt_hybrid_0}" "${_rt_hybrid_1}" "${_rt_hybrid_eligibility}" "${_rt_hybrid_compose}" "${_rt_hybrid_guides}" "${_rt_hybrid_history}" "${_rt_hybrid_reconstruct}" "${_rt_hybrid_blur}" "${_rt_native_reconstruct}" "${_rt_advanced_pack}")
    add_dependencies(${PROJECT_NAME} RTDirectLightingDXIL)
    list(APPEND REDSTONE_DXIL_FILES "${_redstone_directional}" "${_shadow_temporal}" "${_hybrid_source}" "${_rt_hybrid_0}" "${_rt_hybrid_1}" "${_rt_hybrid_eligibility}" "${_rt_hybrid_compose}" "${_rt_hybrid_guides}" "${_rt_hybrid_history}" "${_rt_hybrid_reconstruct}" "${_rt_hybrid_blur}" "${_rt_native_reconstruct}" "${_rt_advanced_pack}")
    register_feature_payload(RTDirectLighting FILES ${REDSTONE_DXIL_FILES} DESTINATION "Shaders/RTDirectLighting")
    list(APPEND REDSTONE_DXIL_CORE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/Core/Shaders/RTDirectLighting"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${REDSTONE_DXIL_FILES} "${CMAKE_CURRENT_BINARY_DIR}/Core/Shaders/RTDirectLighting")
else()
    message(WARNING "DXC was not found: RTDirectLighting directional traversal bytecode will not be packaged; native shadows remain available.")
endif()
