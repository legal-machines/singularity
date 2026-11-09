# A colleague's document review, end to end: the real review run, the real
# Document Intelligence layer, the Matter's real .lm, the real thread and
# decisions - with only the model's replies scripted. Built from exactly the
# Playbook colleague's test sources (playbook_agent_tests, which the line
# before this one's include defines), with the review's own libraries.
if(TARGET playbook_agent_tests)
  get_target_property(lm_document_review_test_sources playbook_agent_tests SOURCES)
  list(FILTER lm_document_review_test_sources EXCLUDE REGEX "AgentProduction\.test\.cpp$")
  add_executable(document_review_tests
    "${NATIVE_APP_SOURCE_DIR}/DocumentReview/DocumentReviewRun/DocumentReviewRun.test.cpp"
    ${lm_document_review_test_sources})
  foreach(lm_document_review_test_property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS
      LINK_LIBRARIES CXX_STANDARD CXX_STANDARD_REQUIRED)
    get_target_property(lm_document_review_test_value playbook_agent_tests ${lm_document_review_test_property})
    if(lm_document_review_test_value)
      set_target_properties(document_review_tests PROPERTIES
        ${lm_document_review_test_property} "${lm_document_review_test_value}")
    endif()
  endforeach()
  target_link_libraries(document_review_tests PRIVATE lm_document_review)
  target_compile_definitions(document_review_tests PRIVATE
    "SINGULARITY_TEST_FIXTURES_DIR=\"${NATIVE_APP_CODE_ROOT}/tests/fixtures\"")
  if(MSVC)
    # main() is one 3,400-line function: a Debug build gives it a frame so
    # large that a review's deeper calls passed MSVC's default 1 MiB
    # main-thread stack (0xC00000FD after 245 s, 2026-11-05). The 8 MiB
    # macOS and Linux give a main thread, as chat_execution_tests has.
    target_link_options(document_review_tests PRIVATE /STACK:8388608)
  endif()
  add_test(NAME document_review COMMAND document_review_tests)
  singularity_label_test(document_review acceptance document)

  # A colleague's work as it runs today (DocumentReview/AssignmentRun): the
  # real run, the real Matter and thread, with only the model's replies scripted
  # - each path pinned before the colleague's path and a member's request share
  #   one model-call contract. Built as the review's test is.
  add_executable(assignment_run_tests
    "${NATIVE_APP_SOURCE_DIR}/DocumentReview/AssignmentRun/AssignmentRun.test.cpp"
    ${lm_document_review_test_sources})
  foreach(lm_document_review_test_property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS
      LINK_LIBRARIES CXX_STANDARD CXX_STANDARD_REQUIRED)
    get_target_property(lm_document_review_test_value playbook_agent_tests ${lm_document_review_test_property})
    if(lm_document_review_test_value)
      set_target_properties(assignment_run_tests PROPERTIES
        ${lm_document_review_test_property} "${lm_document_review_test_value}")
    endif()
  endforeach()
  target_link_libraries(assignment_run_tests PRIVATE lm_document_review)
  target_compile_definitions(assignment_run_tests PRIVATE
    "SINGULARITY_TEST_FIXTURES_DIR=\"${NATIVE_APP_CODE_ROOT}/tests/fixtures\"")
  if(MSVC)
    target_link_options(assignment_run_tests PRIVATE /STACK:8388608)
  endif()
  add_test(NAME assignment_run COMMAND assignment_run_tests)
  singularity_label_test(assignment_run unit document)

  # One model call (Routing/ModelCall, M1c): through the executor only, a stop
  # honoured, each failure classed once, a reply held to its form - with only
  # the model's replies scripted, built as the review's tests are until Routing
  # has a library of its own.
  add_executable(call_model_tests
    "${NATIVE_APP_SOURCE_DIR}/Routing/ModelCall/CallModel.test.cpp"
    ${lm_document_review_test_sources})
  foreach(lm_document_review_test_property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS
      LINK_LIBRARIES CXX_STANDARD CXX_STANDARD_REQUIRED)
    get_target_property(lm_document_review_test_value playbook_agent_tests ${lm_document_review_test_property})
    if(lm_document_review_test_value)
      set_target_properties(call_model_tests PROPERTIES
        ${lm_document_review_test_property} "${lm_document_review_test_value}")
    endif()
  endforeach()
  target_link_libraries(call_model_tests PRIVATE lm_document_review)
  if(MSVC)
    target_link_options(call_model_tests PRIVATE /STACK:8388608)
  endif()
  add_test(NAME call_model COMMAND call_model_tests)
  singularity_label_test(call_model unit)
endif()
