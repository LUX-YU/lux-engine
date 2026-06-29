# INCLUDE GUARD
if(_FUNCTION_SHADERS_INCLUDED_)
	return()
endif()
set(_FUNCTION_SHADERS_INCLUDED_ TRUE)

if(NOT Vulkan_GLSLC_EXECUTABLE)
	find_package(Vulkan REQUIRED)
endif()

#[[
	compile_glsl_shaders(
		OUTPUT_DIR <output_dir>
		OUTPUT_SPIR_V <output_spir_v>
		TARGET <target_name>
		INCLUDE_DIRS <dir1> <dir2> ...    # Optional: directories for #include resolution
		SHADERS <shader1> <shader2> ...
	)
]]
function(compile_glsl_shaders)
	set(_options)
    set(_one_value_arguments OUTPUT_DIR OUTPUT_SPIR_V TARGET)
    set(_multi_value_arguments SHADERS INCLUDE_DIRS)

	cmake_parse_arguments(
		ARGS
		"${_options}"
		"${_one_value_arguments}"
		"${_multi_value_arguments}"
		${ARGN}
	)

	if(NOT ARGS_OUTPUT_DIR)
		set(ARGS_OUTPUT_DIR ${CMAKE_BINARY_DIR}/spir_v)
	endif()

	if(NOT ARGS_OUTPUT_SPIR_V)
		message(FATAL_ERROR "Variable: OUTPUT_SPIR_V must be specified.")
	endif()

	if(NOT ARGS_TARGET)
		message(FATAL_ERROR "Variable: TARGET must be specified.")
	endif()

	# Create output directory
	file(MAKE_DIRECTORY ${ARGS_OUTPUT_DIR})

	# Build include-dir arguments for glslc (supports Google #include extension)
	set(_INCLUDE_ARGS)
	set(_INCLUDE_DEPS)
	foreach(_inc_dir ${ARGS_INCLUDE_DIRS})
		list(APPEND _INCLUDE_ARGS "-I${_inc_dir}")
		# Glob all .glsl headers so changes trigger recompilation
		file(GLOB_RECURSE _inc_files "${_inc_dir}/*.glsl")
		list(APPEND _INCLUDE_DEPS ${_inc_files})
	endforeach()

	set(OUTPUT_FILE_LIST)
	foreach(shader ${ARGS_SHADERS})
		get_filename_component(
			SHADER_NAME
			${shader}
			NAME
		)

		get_filename_component(
			SHADER_ABSOLUTE_PATH
			${shader}
			ABSOLUTE
		)

		set(OUTPUT_FILE_NAME ${ARGS_OUTPUT_DIR}/${SHADER_NAME}.spv)

		# Add custom command for each shader
		add_custom_command(
			OUTPUT ${OUTPUT_FILE_NAME}
			COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.2 ${_INCLUDE_ARGS} ${SHADER_ABSOLUTE_PATH} -o ${OUTPUT_FILE_NAME}
			DEPENDS ${SHADER_ABSOLUTE_PATH} ${_INCLUDE_DEPS}
			COMMENT "Compiling GLSL shader ${SHADER_NAME} (target: vulkan1.2)"
			VERBATIM
		)

		list(APPEND OUTPUT_FILE_LIST ${OUTPUT_FILE_NAME})
	endforeach()

	# Create a custom target to manage shader compilation
	# If the target already exists, don't recreate it
	if(NOT TARGET ${ARGS_TARGET})
		add_custom_target(${ARGS_TARGET}
			DEPENDS ${OUTPUT_FILE_LIST}
			COMMENT "Compiling GLSL shaders for target ${ARGS_TARGET}"
		)
	else()
		# If target already exists, add dependency
		add_dependencies(${ARGS_TARGET} ${OUTPUT_FILE_LIST})
	endif()

	set(${ARGS_OUTPUT_SPIR_V} ${OUTPUT_FILE_LIST} PARENT_SCOPE)
endfunction()

#[[
	compile_and_pack_shaders(
		COMPILE_TARGET  <target>        # custom target driving GLSL compilation
		ATTACH_TO       <target>        # C++ target that depends on this pipeline
		SHADER_DIR      <dir>           # base directory; SHADERS entries are bare filenames
		ASSET_OUT_DIR   <dir>           # output root for .luxasset files (default: ${CMAKE_BINARY_DIR}/assets)
		OUTPUT_SPIR_V   <var>           # [out] list of generated .spv absolute paths
		INCLUDE_DIRS    <dir> ...       # [optional] forwarded as glslc -I flags
		SHADERS         <name> ...      # bare filenames, e.g. fr.vert  grid.frag
	)

	For each bare name "foo.vert":
	  Compiles -> ${CMAKE_BINARY_DIR}/spir_v/foo.vert.spv
	  Packs    -> ${ASSET_OUT_DIR}/shaders/foo.vert.luxasset
	OUTPUT_SPIR_V is set in the caller's scope as intermediate build outputs.
]]
function(compile_and_pack_shaders)
	set(_one_value_args COMPILE_TARGET ATTACH_TO SHADER_DIR ASSET_OUT_DIR OUTPUT_SPIR_V)
	set(_multi_value_args SHADERS INCLUDE_DIRS)
	cmake_parse_arguments(ARGS "" "${_one_value_args}" "${_multi_value_args}" ${ARGN})

	if(NOT ARGS_COMPILE_TARGET)
		message(FATAL_ERROR "[compile_and_pack_shaders] COMPILE_TARGET is required")
	endif()
	if(NOT ARGS_ATTACH_TO)
		message(FATAL_ERROR "[compile_and_pack_shaders] ATTACH_TO is required")
	endif()
	if(NOT ARGS_SHADER_DIR)
		message(FATAL_ERROR "[compile_and_pack_shaders] SHADER_DIR is required")
	endif()
	if(NOT ARGS_OUTPUT_SPIR_V)
		message(FATAL_ERROR "[compile_and_pack_shaders] OUTPUT_SPIR_V is required")
	endif()
	if(NOT ARGS_ASSET_OUT_DIR)
		set(ARGS_ASSET_OUT_DIR "${CMAKE_BINARY_DIR}/assets")
	endif()

	# Expand bare names to absolute source paths
	set(_full_paths)
	foreach(_name ${ARGS_SHADERS})
		list(APPEND _full_paths "${ARGS_SHADER_DIR}/${_name}")
	endforeach()

	# Compile GLSL -> SPIR-V (reuse existing function)
	compile_glsl_shaders(
		TARGET		${ARGS_COMPILE_TARGET}
		OUTPUT_SPIR_V	_spv_list
		INCLUDE_DIRS	${ARGS_INCLUDE_DIRS}
		SHADERS		${_full_paths}
	)

	# Derive add_asset ENTRIES from bare names.
	# Use the basename for the .spv path (compile_glsl_shaders strips directories)
	# and the full relative path for the .luxasset output (preserves subdirectory structure).
	set(_entries)
	foreach(_name ${ARGS_SHADERS})
		get_filename_component(_base_name ${_name} NAME)
		list(APPEND _entries "shader|${CMAKE_BINARY_DIR}/spir_v/${_base_name}.spv|shaders/${_name}.luxasset")
	endforeach()

	# Register asset pack and attach it to the C++ target
	set(_asset_obj "${ARGS_COMPILE_TARGET}_assets")
	add_asset(
		NAME	${_asset_obj}
		OUT_DIR	${ARGS_ASSET_OUT_DIR}
		ENTRIES	${_entries}
	)
	target_add_asset(
		TARGET	${ARGS_ATTACH_TO}
		NAME	${_asset_obj}
	)

	# Ensure shaders are compiled before the C++ target links
	add_dependencies(${ARGS_ATTACH_TO} ${ARGS_COMPILE_TARGET})

	# Return .spv list to caller scope (intermediate build outputs)
	set(${ARGS_OUTPUT_SPIR_V} ${_spv_list} PARENT_SCOPE)
endfunction()

#[[
	compile_and_pack_shader_variants(
		COMPILE_TARGET <target>        # custom target driving GLSL compilation
		ATTACH_TO      <target>        # C++ target that depends on this pipeline
		SHADER_DIR     <dir>           # base directory; SHADER is a bare relative path
		ASSET_OUT_DIR  <dir>           # output root for .luxasset files
		OUTPUT_SPIR_V  <var>           # [out] list of generated .spv absolute paths (appended)
		INCLUDE_DIRS   <dir> ...       # [optional] forwarded as glslc -I flags
		SHADER         <relpath>       # single source, e.g. deferred/deferred_lighting.frag
		VARIANTS                       # list of "suffix|define1;define2;..."
			"pcf|LUX_SHADOW_SAMPLER_HEADER=\"shadow_pcf.glsl\""
			"evsm|LUX_SHADOW_SAMPLER_HEADER=\"shadow_evsm.glsl\""
	)

	For each variant "<suffix>|<defines>":
	  Compiles `${SHADER_DIR}/${SHADER}` with `-D<defines>` →
	  ${CMAKE_BINARY_DIR}/spir_v/<basename>.<suffix>.spv
	  Packs    → ${ASSET_OUT_DIR}/shaders/<SHADER>.<suffix>.luxasset

	Designed for shadow-technique-style polymorphism (see
	.internal/plan/evsm-shadow-implementation-guide.md): a single .frag source
	produces multiple SPIR-V variants, each selecting a different
	`shadow_<technique>.glsl` via LUX_SHADOW_SAMPLER_HEADER.
]]
function(compile_and_pack_shader_variants)
	set(_one_value_args COMPILE_TARGET ATTACH_TO SHADER_DIR ASSET_OUT_DIR OUTPUT_SPIR_V SHADER)
	set(_multi_value_args VARIANTS INCLUDE_DIRS)
	cmake_parse_arguments(ARGS "" "${_one_value_args}" "${_multi_value_args}" ${ARGN})

	if(NOT ARGS_COMPILE_TARGET)
		message(FATAL_ERROR "[compile_and_pack_shader_variants] COMPILE_TARGET is required")
	endif()
	if(NOT ARGS_ATTACH_TO)
		message(FATAL_ERROR "[compile_and_pack_shader_variants] ATTACH_TO is required")
	endif()
	if(NOT ARGS_SHADER_DIR)
		message(FATAL_ERROR "[compile_and_pack_shader_variants] SHADER_DIR is required")
	endif()
	if(NOT ARGS_SHADER)
		message(FATAL_ERROR "[compile_and_pack_shader_variants] SHADER is required")
	endif()
	if(NOT ARGS_VARIANTS)
		message(FATAL_ERROR "[compile_and_pack_shader_variants] VARIANTS is required")
	endif()
	if(NOT ARGS_ASSET_OUT_DIR)
		set(ARGS_ASSET_OUT_DIR "${CMAKE_BINARY_DIR}/assets")
	endif()

	set(_shader_abs   "${ARGS_SHADER_DIR}/${ARGS_SHADER}")
	get_filename_component(_shader_basename "${ARGS_SHADER}" NAME)
	set(_spv_dir "${CMAKE_BINARY_DIR}/spir_v")
	file(MAKE_DIRECTORY "${_spv_dir}")

	# Aggregate include-dir args + include-file deps (matches compile_glsl_shaders).
	set(_inc_args)
	set(_inc_deps)
	foreach(_inc_dir ${ARGS_INCLUDE_DIRS})
		list(APPEND _inc_args "-I${_inc_dir}")
		file(GLOB_RECURSE _glob_inc "${_inc_dir}/*.glsl")
		list(APPEND _inc_deps ${_glob_inc})
	endforeach()

	set(_spv_outputs)
	set(_asset_entries)

	foreach(_variant_spec ${ARGS_VARIANTS})
		# Parse "suffix|define1;define2;..."
		string(FIND "${_variant_spec}" "|" _pipe_pos)
		if(_pipe_pos LESS 0)
			message(FATAL_ERROR
				"[compile_and_pack_shader_variants] variant spec missing '|' separator: ${_variant_spec}")
		endif()
		string(SUBSTRING "${_variant_spec}" 0 ${_pipe_pos} _suffix)
		math(EXPR _defines_start "${_pipe_pos} + 1")
		string(SUBSTRING "${_variant_spec}" ${_defines_start} -1 _defines_blob)

		# Defines are semicolon-separated; turn each into -DXXX=YYY for glslc.
		set(_define_args)
		string(REPLACE ";" ";" _defines_list "${_defines_blob}")
		foreach(_def IN LISTS _defines_list)
			if(_def)
				list(APPEND _define_args "-D${_def}")
			endif()
		endforeach()

		set(_spv_out "${_spv_dir}/${_shader_basename}.${_suffix}.spv")

		add_custom_command(
			OUTPUT "${_spv_out}"
			COMMAND ${Vulkan_GLSLC_EXECUTABLE}
				--target-env=vulkan1.2
				${_inc_args}
				${_define_args}
				"${_shader_abs}"
				-o "${_spv_out}"
			DEPENDS "${_shader_abs}" ${_inc_deps}
			COMMENT "Compiling GLSL variant ${ARGS_SHADER} [${_suffix}] → ${_shader_basename}.${_suffix}.spv"
			VERBATIM
		)

		list(APPEND _spv_outputs "${_spv_out}")
		list(APPEND _asset_entries "shader|${_spv_out}|shaders/${ARGS_SHADER}.${_suffix}.luxasset")
	endforeach()

	# Wrap the file outputs in a per-call sub-target so add_dependencies()
	# below sees a target name (it does NOT accept raw file paths).
	get_filename_component(_shader_stem_for_subtarget "${ARGS_SHADER}" NAME_WE)
	set(_sub_target "${ARGS_COMPILE_TARGET}_${_shader_stem_for_subtarget}_variants_drv")
	add_custom_target(${_sub_target} DEPENDS ${_spv_outputs})
	if(NOT TARGET ${ARGS_COMPILE_TARGET})
		add_custom_target(${ARGS_COMPILE_TARGET}
			COMMENT "Compiling shader variants for target ${ARGS_COMPILE_TARGET}"
		)
	endif()
	add_dependencies(${ARGS_COMPILE_TARGET} ${_sub_target})

	# Pack to .luxasset (one add_asset() call per variant set — name disambiguated
	# by the shader basename to avoid collisions across multiple sources).
	get_filename_component(_shader_stem "${ARGS_SHADER}" NAME_WE)
	set(_asset_obj "${ARGS_COMPILE_TARGET}_${_shader_stem}_variants")
	add_asset(
		NAME    ${_asset_obj}
		OUT_DIR ${ARGS_ASSET_OUT_DIR}
		ENTRIES ${_asset_entries}
	)
	target_add_asset(
		TARGET ${ARGS_ATTACH_TO}
		NAME   ${_asset_obj}
	)
	add_dependencies(${ARGS_ATTACH_TO} ${ARGS_COMPILE_TARGET})

	# Append .spv outputs to caller-visible variable.
	if(ARGS_OUTPUT_SPIR_V)
		set(_existing "${${ARGS_OUTPUT_SPIR_V}}")
		list(APPEND _existing ${_spv_outputs})
		set(${ARGS_OUTPUT_SPIR_V} "${_existing}" PARENT_SCOPE)
	endif()
endfunction()