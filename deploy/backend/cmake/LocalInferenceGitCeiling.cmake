# Loaded first by local inference's llama.cpp build (CMAKE_PROJECT_INCLUDE_BEFORE,
# LocalInferenceRuntime.cmake). The downloaded source has no .git of its own,
# so llama.cpp's configure-time `git rev-parse` would climb into the
# repository that holds the build tree - Singularity's - and stamp
# llama-server with Singularity's commit. A ceiling just above the source
# keeps git inside it; the upstream revision is recorded in
# dependencies/llama.cpp/README.md.
get_filename_component(_local_inference_source_parent "${CMAKE_SOURCE_DIR}" DIRECTORY)
set(ENV{GIT_CEILING_DIRECTORIES} "${_local_inference_source_parent}")
