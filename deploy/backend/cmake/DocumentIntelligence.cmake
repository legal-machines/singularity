# The Document Intelligence & Preparation Layer (code/backend/DocumentIntelligence):
# a professional document turned into material a model can work on - its
# structure read, the context a task needs prepared, a model's answer checked
# against the source, and a revised draft written as Word tracked changes.
#
# Built as ARCH-8 asks of every new component: its own library, which the
# application links and its tests link, never copies of its sources. What it
# uses that is not yet a module of its own - Document/DocxExtractor,
# Document/OfficeTextEdit and Platform/Sha256 - is compiled by whoever links
# it: the application has them already, and each test compiles them beside it.
set(LM_DOCUMENT_INTELLIGENCE_SOURCES
  DocumentIntelligence/SourceIntake/SourceIntake.cpp
  DocumentIntelligence/SourceIntake/SourceIntake.hpp
  DocumentIntelligence/LegalStructure/LegalStructure.cpp
  DocumentIntelligence/LegalStructure/LegalStructure.hpp
  DocumentIntelligence/ContextPreparation/ContextPreparation.cpp
  DocumentIntelligence/ContextPreparation/ContextPreparation.hpp
  DocumentIntelligence/ReviewResult/ReviewResult.cpp
  DocumentIntelligence/ReviewResult/ReviewResult.hpp
  DocumentIntelligence/RevisionWriter/RevisionWriter.cpp
  DocumentIntelligence/RevisionWriter/RevisionWriter.hpp
  DocumentIntelligence/IntegrityCheck/IntegrityCheck.cpp
  DocumentIntelligence/IntegrityCheck/IntegrityCheck.hpp
  DocumentIntelligence/ChangeAccount/ChangeAccount.cpp
  DocumentIntelligence/ChangeAccount/ChangeAccount.hpp
  DocumentIntelligence/SlideWriter/SlideWriter.cpp
  DocumentIntelligence/SlideWriter/SlideWriter.hpp
  DocumentIntelligence/EmailDraft/EmailDraft.cpp
  DocumentIntelligence/EmailDraft/EmailDraft.hpp
  DocumentIntelligence/MaterialFacts/MaterialFacts.cpp
  DocumentIntelligence/MaterialFacts/MaterialFacts.hpp
  DocumentIntelligence/Types/LegalDocument.hpp
)
list(TRANSFORM LM_DOCUMENT_INTELLIGENCE_SOURCES PREPEND "${NATIVE_APP_SOURCE_DIR}/")
add_library(lm_document_intelligence STATIC ${LM_DOCUMENT_INTELLIGENCE_SOURCES})
target_include_directories(lm_document_intelligence PUBLIC "${NATIVE_APP_SOURCE_DIR}")
target_link_libraries(lm_document_intelligence PUBLIC
  nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc)
target_include_directories(lm_document_intelligence SYSTEM PRIVATE
  "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
  "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
  "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
  "$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>")
if(MSVC)
  target_compile_options(lm_document_intelligence PRIVATE /W4 /permissive- /utf-8)
  target_compile_definitions(lm_document_intelligence PRIVATE NOMINMAX)
else()
  target_compile_options(lm_document_intelligence PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_link_libraries(native_app PRIVATE lm_document_intelligence)

# A colleague's document review in a thread (code/backend/DocumentReview): the
# record of a review and its versions, the lawyer's decisions, and the run that
# carries a review out through the layer above. A library of its own as well;
# it stands on domains that are not modules yet (Matter, Chat, Agent, Routing,
# WorkProduct), which the application compiles and links it with.
set(LM_DOCUMENT_REVIEW_SOURCES
  DocumentReview/Types/DocumentReview.hpp
  DocumentReview/Types/DocumentReviewJson.hpp
  DocumentReview/DocumentReviewActions/DocumentReviewActions.cpp
  DocumentReview/DocumentReviewActions/DocumentReviewActions.hpp
  DocumentReview/DocumentReviewRun/DocumentReviewRun.cpp
  DocumentReview/DocumentReviewRun/DocumentReviewRun.hpp
  DocumentReview/ColleagueWork/ColleagueWork.cpp
  DocumentReview/ColleagueWork/ColleagueWork.hpp
  DocumentReview/MaterialCheck/MaterialCheck.cpp
  DocumentReview/MaterialCheck/MaterialCheck.hpp
  DocumentReview/AssignmentRun/AssignmentRun.cpp
  DocumentReview/AssignmentRun/AssignmentRun.hpp
  DocumentReview/ColleagueTalk/ColleagueTalk.cpp
  DocumentReview/ColleagueTalk/ColleagueTalk.hpp
)
list(TRANSFORM LM_DOCUMENT_REVIEW_SOURCES PREPEND "${NATIVE_APP_SOURCE_DIR}/")
add_library(lm_document_review STATIC ${LM_DOCUMENT_REVIEW_SOURCES})
target_include_directories(lm_document_review PUBLIC "${NATIVE_APP_SOURCE_DIR}")
target_link_libraries(lm_document_review PUBLIC lm_document_intelligence
  nlohmann_json::nlohmann_json libzip::zip pugixml::pugixml utf8proc native_app_gliner2_bridge_library)
target_compile_definitions(lm_document_review PRIVATE
  "NATIVE_APP_DISPLAY_NAME=\"${NATIVE_APP_DISPLAY_NAME}\""
  "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\"")
if(MSVC)
  target_compile_options(lm_document_review PRIVATE /W4 /permissive- /utf-8)
  target_compile_definitions(lm_document_review PRIVATE NOMINMAX)
else()
  target_compile_options(lm_document_review PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_link_libraries(native_app PRIVATE lm_document_review)

if(BUILD_TESTING)
  # Each component's own test, on the synthetic NDA of
  # code/tests/fixtures/synthetic_nda (generate.py writes it).
  function(lm_document_intelligence_test target test_name)
    singularity_add_executable(${target} ${ARGN}
      Document/DocxExtractor/DocxExtractor.cpp
      Document/OfficeTextEdit/OfficeTextEdit.cpp
      Platform/Sha256/Sha256.cpp)
    target_include_directories(${target} PRIVATE "${NATIVE_APP_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE lm_document_intelligence)
    target_include_directories(${target} SYSTEM PRIVATE
      "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>"
      "$<TARGET_PROPERTY:zip,INTERFACE_INCLUDE_DIRECTORIES>"
      "$<TARGET_PROPERTY:pugixml,INTERFACE_INCLUDE_DIRECTORIES>"
      "$<TARGET_PROPERTY:utf8proc,INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_definitions(${target} PRIVATE
      "NATIVE_APP_SLUG=\"${NATIVE_APP_SLUG}\""
      "SINGULARITY_TEST_FIXTURES_DIR=\"${NATIVE_APP_CODE_ROOT}/tests/fixtures\"")
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
      target_compile_definitions(${target} PRIVATE NOMINMAX)
    else()
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ${test_name} COMMAND ${target})
    set_tests_properties(${test_name} PROPERTIES LABELS "unit;document")
  endfunction()

  lm_document_intelligence_test(document_intelligence_source_intake_tests document_intelligence_source_intake
    DocumentIntelligence/SourceIntake/SourceIntake.test.cpp)
  lm_document_intelligence_test(document_intelligence_legal_structure_tests document_intelligence_legal_structure
    DocumentIntelligence/LegalStructure/LegalStructure.test.cpp)
  lm_document_intelligence_test(document_intelligence_context_preparation_tests document_intelligence_context_preparation
    DocumentIntelligence/ContextPreparation/ContextPreparation.test.cpp)
  lm_document_intelligence_test(document_intelligence_review_result_tests document_intelligence_review_result
    DocumentIntelligence/ReviewResult/ReviewResult.test.cpp)
  lm_document_intelligence_test(document_intelligence_revision_writer_tests document_intelligence_revision_writer
    DocumentIntelligence/RevisionWriter/RevisionWriter.test.cpp)
  lm_document_intelligence_test(document_intelligence_integrity_check_tests document_intelligence_integrity_check
    DocumentIntelligence/IntegrityCheck/IntegrityCheck.test.cpp)
  lm_document_intelligence_test(document_intelligence_change_account_tests document_intelligence_change_account
    DocumentIntelligence/ChangeAccount/ChangeAccount.test.cpp)
  # A revised deck written and read back as the Matter reads decks.
  lm_document_intelligence_test(document_intelligence_slide_writer_tests document_intelligence_slide_writer
    DocumentIntelligence/SlideWriter/SlideWriter.test.cpp
    Document/PptxExtractor/PptxExtractor.cpp)
  # An email draft written as a file a mail program opens, and read back.
  lm_document_intelligence_test(document_intelligence_email_draft_tests document_intelligence_email_draft
    DocumentIntelligence/EmailDraft/EmailDraft.test.cpp)
  # The facts documents share, checked against each other with the values read
  # as Papinian Guard computes with them.
  lm_document_intelligence_test(document_intelligence_material_facts_tests document_intelligence_material_facts
    DocumentIntelligence/MaterialFacts/MaterialFacts.test.cpp
    PapinianGuard/SemanticAlgebra/SemanticAlgebra.cpp
    Context/TemporalSemantics/TemporalSemantics.cpp)

  # A colleague's talk (the owner, 2026-10-26): a light call first, and only
  # what the colleague asks for after it.
  lm_document_intelligence_test(document_review_colleague_talk_tests document_review_colleague_talk
    DocumentReview/ColleagueTalk/ColleagueTalk.test.cpp
    DocumentReview/ColleagueTalk/ColleagueTalk.cpp)

  # The layer measured: one review asked as the extractor reads the document
  # and as the layer prepares it, scored against the fixture's answer key.
  # Offline it writes the requests; the benchmark's driver sends them, and
  # the next run scores the answers.
  lm_document_intelligence_test(document_review_benchmark_runner document_review_benchmark
    ../tests/document_review_benchmark.cpp
    Agent/ColleagueBrief/ColleagueBrief.cpp)
  set_tests_properties(document_review_benchmark PROPERTIES LABELS "benchmark;document")
endif()

# Every other build that compiles the Native Bridge (the command-line and
# acceptance builds made from NATIVE_APP_SOURCES, on every system) takes the
# review with it, as the application does: once all of this directory's
# targets exist.
function(lm_document_review_join_bridge_builds)
  get_property(lm_document_review_targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(lm_document_review_target IN LISTS lm_document_review_targets)
    if(lm_document_review_target STREQUAL "native_app")
      continue()
    endif()
    get_target_property(lm_document_review_type ${lm_document_review_target} TYPE)
    if(NOT lm_document_review_type STREQUAL "EXECUTABLE")
      continue()
    endif()
    get_target_property(lm_document_review_sources ${lm_document_review_target} SOURCES)
    if(lm_document_review_sources MATCHES "Bridge/(NativeBridge/NativeBridge|DocumentReviewBridge/DocumentReviewBridge)\.cpp")
      target_link_libraries(${lm_document_review_target} PRIVATE lm_document_review)
    endif()
  endforeach()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL lm_document_review_join_bridge_builds)
