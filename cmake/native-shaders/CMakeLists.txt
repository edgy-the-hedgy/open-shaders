cmake_minimum_required(VERSION 3.25)
project(NativeShaderValidation LANGUAGES NONE)

# Reuse production shader build commands without configuring the Skyrim plugin.
get_filename_component(
    NATIVE_SHADER_SOURCE_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    ABSOLUTE
)
set(NATIVE_SHADER_VALIDATION_FLAGS -WX)
add_custom_target(${PROJECT_NAME})
include("../DX12NativeShadow.cmake")
include("../DX12NativeComposite.cmake")
if(NOT TARGET DX12NativeShadowDXIL OR NOT TARGET DX12NativeCompositeDXIL)
    message(FATAL_ERROR "DXC is required for native shader validation")
endif()
add_custom_target(
    native_shader_validation
    ALL
    DEPENDS DX12NativeShadowDXIL DX12NativeCompositeDXIL
)

# Compile the v2 producer/consumer storage primitives in isolation. This
# deliberately does not stage a runtime DXIL file or activate a second pool.
set(_specialized_test "${NATIVE_SHADER_SOURCE_ROOT}/tests/shaders/NativeSpecializedMaterialValidation.hlsl")
set(_specialized_include "${NATIVE_SHADER_SOURCE_ROOT}/features/Native Composite/Shaders/NativeComposite")
set(_specialized_outputs)
foreach(_entry IN ITEMS publishMain readMain)
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/SpecializedMaterial-${_entry}.dxil")
    add_custom_command(
        OUTPUT "${_out}"
        COMMAND "${NATIVE_SHADOW_DXC}" -T cs_6_5 -E ${_entry} -HV 2021 -O3 -WX -Ges -Gis
            -I "${_specialized_include}" -Fo "${_out}" "${_specialized_test}"
        DEPENDS "${_specialized_test}" "${_specialized_include}/NativeSpecializedMaterial.hlsli"
        VERBATIM
    )
    list(APPEND _specialized_outputs "${_out}")
endforeach()
add_custom_target(native_specialized_material_validation DEPENDS ${_specialized_outputs})
add_dependencies(native_shader_validation native_specialized_material_validation)
