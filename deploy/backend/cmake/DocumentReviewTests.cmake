# A colleague's document review, end to end (DocumentReview/DocumentReviewRun,
# D-091): the real review run, the real Document Intelligence layer, the
# Matter's real .lm, the real thread and decisions - with only the model's
# replies scripted. Built from exactly the Playbook colleague's test sources
# (playbook_agent_tests, which the line before this one's include defines),
# with the review's own libraries.
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
endif()
