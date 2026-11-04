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
            "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/NativeTraceSampling.hlsli"
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
    file(GLOB_RECURSE _native_hybrid_dependencies CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/*.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders/Common/*.hlsli")
    set(_native_hybrid_files "")
    foreach(_hybrid_pass IN ITEMS gi occlusion)
        if(_hybrid_pass STREQUAL "gi")
            set(_hybrid_name hybridGI)
        else()
            set(_hybrid_name hybridAO)
        endif()
        set(_hybrid_file "${REDSTONE_DXIL_DIRECTORY}/${_hybrid_name}.dxil")
        add_custom_command(OUTPUT "${_hybrid_file}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
            COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2018 -O3 -Ges -D DX12_HYBRID_GI=1
                -I "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders"
                -I "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders"
                -Fo "${_hybrid_file}"
                "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/${_hybrid_pass}.cs.hlsl"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RedstoneGI/Shaders/RedstoneGI/${_hybrid_pass}.cs.hlsl"
                ${_native_hybrid_dependencies}
            VERBATIM)
        list(APPEND _native_hybrid_files "${_hybrid_file}")
    endforeach()
    set(_native_signal_preparation "${REDSTONE_DXIL_DIRECTORY}/nativeSignalPreparation.dxil")
    add_custom_command(OUTPUT "${_native_signal_preparation}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
        COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2018 -O3 -Ges
            -I "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders"
            -Fo "${_native_signal_preparation}"
            "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/nativeSignalPreparation.hlsl6"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/nativeSignalPreparation.hlsl6"
            ${_native_hybrid_dependencies}
        VERBATIM)
    list(APPEND _native_hybrid_files "${_native_signal_preparation}")
    foreach(_native_composition IN ITEMS nativeDirectCompose nativeHybridCompose)
        set(_native_composition_file "${REDSTONE_DXIL_DIRECTORY}/${_native_composition}.dxil")
        add_custom_command(OUTPUT "${_native_composition_file}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${REDSTONE_DXIL_DIRECTORY}"
            COMMAND "${REDSTONE_DXC}" -T cs_6_5 -E main -HV 2018 -O3 -Ges
                -I "${CMAKE_CURRENT_SOURCE_DIR}/package/Shaders"
                -Fo "${_native_composition_file}"
                "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/${_native_composition}.hlsl6"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/features/RTDirectLighting/Shaders/RTDirectLighting/${_native_composition}.hlsl6"
                ${_native_hybrid_dependencies}
            VERBATIM)
        list(APPEND _native_hybrid_files "${_native_composition_file}")
    endforeach()
    add_custom_target(NativeHybridDXIL DEPENDS ${_native_hybrid_files})
    add_dependencies(${PROJECT_NAME} NativeHybridDXIL)
    list(APPEND REDSTONE_DXIL_FILES ${_native_hybrid_files})
    add_custom_target(RTDirectLightingDXIL DEPENDS "${_redstone_directional}")
    add_dependencies(${PROJECT_NAME} RTDirectLightingDXIL)
    list(APPEND REDSTONE_DXIL_FILES "${_redstone_directional}")
    register_feature_payload(RTDirectLighting FILES ${REDSTONE_DXIL_FILES} DESTINATION "Shaders/RTDirectLighting")
    list(APPEND REDSTONE_DXIL_CORE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/Core/Shaders/RTDirectLighting"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${REDSTONE_DXIL_FILES} "${CMAKE_CURRENT_BINARY_DIR}/Core/Shaders/RTDirectLighting")
else()
    message(WARNING "DXC was not found: RTDirectLighting directional traversal bytecode will not be packaged; native shadows remain available.")
endif()
