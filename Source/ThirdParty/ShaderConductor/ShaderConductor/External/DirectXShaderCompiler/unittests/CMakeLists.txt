add_custom_target(UnitTests)
set_target_properties(UnitTests PROPERTIES FOLDER "Tests")

if (APPLE)
  #UE Change Begin: Replace rpath with 'install_name_tool' after deployment
  set(CMAKE_INSTALL_RPATH "RPATH_DUMMY")
  #UE Change End: Replace rpath with 'install_name_tool' after deployment
else(UNIX)
  set(CMAKE_INSTALL_RPATH "\$ORIGIN/../../lib${LLVM_LIBDIR_SUFFIX}")
endif()

function(add_llvm_unittest test_dirname)
  add_unittest(UnitTests ${test_dirname} ${ARGN})
endfunction()

add_subdirectory(ADT)
add_subdirectory(Analysis)
add_subdirectory(AsmParser)
add_subdirectory(Bitcode)
# add_subdirectory(CodeGen) - HLSL doesn't codegen...
# add_subdirectory(DebugInfo) - HLSL doesn't generate dwarf
add_subdirectory(DxcSupport)
# add_subdirectory(ExecutionEngine) - HLSL Change - removed
add_subdirectory(IR)
# add_subdirectory(LineEditor) - HLSL Change - removed
add_subdirectory(Linker)
# add_subdirectory(MC) - HLSL doesn't codegen...
add_subdirectory(Option)
add_subdirectory(ProfileData)
add_subdirectory(Support)
add_subdirectory(Transforms)
