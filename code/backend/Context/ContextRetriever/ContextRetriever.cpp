#include "Platform/Trace/Trace.hpp"
#include "Context/ContextRetriever/ContextRetriever.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace singularity::context {
namespace {

struct RankedChunk {
  const RetrievalDocument *document = nullptr;
  const documents::DocumentChunk *chunk = nullptr;
  std::size_t document_order = 0;
  std::size_t chunk_order = 0;
  double score = 0.0;
  double document_name_score = 0.0;
};

bool decode_utf8(const std::string &text, std::size_t &offset,
                 std::uint32_t &code_point) {
  const auto first = static_cast<unsigned char>(text[offset]);
  std::size_t length = 0;
  if (first < 0x80U) {
    code_point = first;
    ++offset;
    return true;
  }
  if ((first & 0xe0U) == 0xc0U) {
    code_point = first & 0x1fU;
    length = 2;
  } else if ((first & 0xf0U) == 0xe0U) {
    code_point = first & 0x0fU;
    length = 3;
  } else if ((first & 0xf8U) == 0xf0U) {
    code_point = first & 0x07U;
    length = 4;
  } else {
    ++offset;
    return false;
  }
  if (offset + length > text.size()) {
    offset = text.size();
    return false;
  }
  for (std::size_t index = 1; index < length; ++index) {
    const auto next = static_cast<unsigned char>(text[offset + index]);
    if ((next & 0xc0U) != 0x80U) {
      ++offset;
      return false;
    }
    code_point = (code_point << 6U) | (next & 0x3fU);
  }
  offset += length;
  return true;
}

void append_utf8(std::string &target, std::uint32_t code_point) {
  if (code_point <= 0x7fU) {
    target.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7ffU) {
    target.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
    target.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
  } else if (code_point <= 0xffffU) {
    target.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
    target.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
    target.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
  } else {
    target.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
    target.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
    target.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
    target.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
  }
}

std::uint32_t lowercase(std::uint32_t code_point) {
  if (code_point >= 'A' && code_point <= 'Z') return code_point + 32U;
  if (code_point >= 0x0410U && code_point <= 0x042fU) return code_point + 32U;
  if (code_point == 0x0401U) return 0x0451U;
  return code_point;
}

bool word_character(std::uint32_t code_point) {
  if ((code_point >= 'a' && code_point <= 'z') ||
      (code_point >= 'A' && code_point <= 'Z') ||
      (code_point >= '0' && code_point <= '9') || code_point == '_') {
    return true;
  }
  // The local V1 corpus is English/Russian. Treat Cyrillic scalars as word
  // characters explicitly instead of applying locale-dependent C functions.
  return (code_point >= 0x0400U && code_point <= 0x052fU);
}

std::vector<std::string> tokenize(const std::string &text) {
  std::vector<std::string> tokens;
  std::string current;
  for (std::size_t offset = 0; offset < text.size();) {
    std::uint32_t code_point = 0;
    if (!decode_utf8(text, offset, code_point) || !word_character(code_point)) {
      if (!current.empty()) {
        tokens.push_back(std::move(current));
        current.clear();
      }
      continue;
    }
    append_utf8(current, lowercase(code_point));
  }
  if (!current.empty()) tokens.push_back(std::move(current));
  return tokens;
}

std::unordered_map<std::string, std::size_t>
frequencies(const std::vector<std::string> &tokens) {
  std::unordered_map<std::string, std::size_t> result;
  for (const auto &token : tokens) ++result[token];
  return result;
}

bool rank_before(const RankedChunk &left, const RankedChunk &right) {
  if (std::abs(left.score - right.score) > 1e-12) {
    return left.score > right.score;
  }
  if (left.document_order != right.document_order) {
    return left.document_order < right.document_order;
  }
  return left.chunk_order < right.chunk_order;
}

} // namespace

std::vector<RetrievedContextChunk>
ContextRetriever::retrieve(const ContextRetrievalRequest &request) {
  if (request.maximum_chunks == 0) return {};
  platform::trace::Span span("evidence", "retriever.rank", {{"documents", request.documents.size()},
      {"maximumChunks", request.maximum_chunks}});
  if (span.active()) {
    std::size_t chunks = 0;
    std::size_t bytes = 0;
    for (const auto &document : request.documents) {
      chunks += document.chunks.size();
      for (const auto &chunk : document.chunks) bytes += chunk.text.size();
    }
    span.add("candidateChunks", chunks);
    span.add("candidateBytes", bytes);
  }

  std::unordered_map<std::string, std::size_t> document_positions;
  for (std::size_t index = 0; index < request.documents.size(); ++index) {
    const auto &id = request.documents[index].document_id;
    if (id.empty() || !document_positions.emplace(id, index).second) {
      throw std::invalid_argument("Retrieval document IDs must be unique and non-empty");
    }
  }

  std::unordered_set<std::string> selected_seen;
  std::vector<std::size_t> selected_positions;
  for (const auto &id : request.selected_document_ids) {
    if (id.empty() || !selected_seen.insert(id).second) {
      throw std::invalid_argument("Selected document IDs must be unique and non-empty");
    }
    const auto found = document_positions.find(id);
    if (found == document_positions.end()) {
      throw std::runtime_error("A selected source is not available for local retrieval: " + id);
    }
    if (request.documents[found->second].chunks.empty()) {
      throw std::runtime_error("A selected source has no AI-ready local chunks: " + id);
    }
    selected_positions.push_back(found->second);
  }

  const bool explicit_selection = !selected_positions.empty();
  std::vector<std::size_t> candidate_positions;
  if (explicit_selection) {
    candidate_positions = selected_positions;
  } else {
    candidate_positions.reserve(request.documents.size());
    for (std::size_t index = 0; index < request.documents.size(); ++index) {
      if (!request.documents[index].chunks.empty()) candidate_positions.push_back(index);
    }
  }

  std::vector<RankedChunk> ranked;
  for (const auto position : candidate_positions) {
    const auto &document = request.documents[position];
    for (std::size_t chunk_index = 0; chunk_index < document.chunks.size();
         ++chunk_index) {
      ranked.push_back(
          {&document, &document.chunks[chunk_index], position, chunk_index, 0.0, 0.0});
    }
  }
  if (ranked.empty()) return {};

  const auto query_tokens = tokenize(request.query);
  const auto query_frequency = frequencies(query_tokens);
  std::unordered_map<std::string, std::size_t> document_frequency;
  double total_length = 0.0;
  std::vector<std::unordered_map<std::string, std::size_t>> body_frequencies;
  body_frequencies.reserve(ranked.size());
  for (const auto &candidate : ranked) {
    auto body = frequencies(tokenize(candidate.chunk->text));
    total_length += static_cast<double>(std::max<std::size_t>(1, tokenize(candidate.chunk->text).size()));
    for (const auto &[token, ignored] : body) {
      (void)ignored;
      if (query_frequency.count(token) > 0) ++document_frequency[token];
    }
    body_frequencies.push_back(std::move(body));
  }

  const auto average_length = total_length / static_cast<double>(ranked.size());
  constexpr double k1 = 1.2;
  constexpr double b = 0.75;
  for (std::size_t index = 0; index < ranked.size(); ++index) {
    const auto &candidate = ranked[index];
    const auto heading_frequency = frequencies(
        tokenize(candidate.chunk->heading.value_or(std::string())));
    const auto name_frequency = frequencies(tokenize(candidate.document->name));
    std::size_t body_length = 0;
    for (const auto &[ignored, count] : body_frequencies[index]) {
      (void)ignored;
      body_length += count;
    }
    for (const auto &[token, query_count] : query_frequency) {
      const auto frequency_found = body_frequencies[index].find(token);
      const double term_frequency = frequency_found == body_frequencies[index].end()
                                        ? 0.0
                                        : static_cast<double>(frequency_found->second);
      const double occurrence_count = static_cast<double>(query_count);
      const double containing = static_cast<double>(document_frequency[token]);
      const double corpus_size = static_cast<double>(ranked.size());
      const double inverse_frequency =
          std::log(1.0 + (corpus_size - containing + 0.5) / (containing + 0.5));
      if (term_frequency > 0.0) {
        const double normalization =
            term_frequency + k1 * (1.0 - b + b *
                (static_cast<double>(std::max<std::size_t>(1, body_length)) /
                 average_length));
        ranked[index].score += occurrence_count * inverse_frequency *
                               (term_frequency * (k1 + 1.0) / normalization);
      }
      if (heading_frequency.count(token) > 0) {
        ranked[index].score += occurrence_count * inverse_frequency * 1.75;
      }
      if (name_frequency.count(token) > 0) {
        const auto signal = occurrence_count * inverse_frequency * 0.75;
        ranked[index].score += signal;
        ranked[index].document_name_score += signal;
      }
    }
  }
  std::stable_sort(ranked.begin(), ranked.end(), rank_before);

  std::vector<const RankedChunk *> chosen;
  chosen.reserve(std::min(request.maximum_chunks, ranked.size()));
  std::unordered_set<std::string> chosen_ids;
  if (explicit_selection) {
    // Preserve the user's selection order while guaranteeing that every
    // selected source contributes its best local chunk when capacity allows.
    for (const auto position : selected_positions) {
      const auto found = std::find_if(
          ranked.begin(), ranked.end(), [position](const RankedChunk &candidate) {
            return candidate.document_order == position;
          });
      if (found != ranked.end() && chosen.size() < request.maximum_chunks) {
        chosen.push_back(&*found);
        chosen_ids.insert(found->chunk->chunk_id);
      }
    }
  } else {
    // A lawyer can name two or more documents in a comparison request even
    // when using Matter-wide relevance mode. Preserve one best chunk from
    // each document whose displayed name actually matches the query before
    // global fill; otherwise repeated terms in one document can crowd the
    // other named instrument out of the eight-chunk window entirely.
    std::unordered_set<std::string> covered_named_documents;
    // A word of exactly one document's name ("tracker" of "Amsterdam DD
    // Tracker.xlsx") names that document alone: the lawyer who writes it
    // means it, so its best passage goes first. Words many names share
    // ("agreement", "nda") name none in particular, and their documents
    // follow; before this, "check the slide against the DD tracker and the
    // supply agreement" filled the window with the Matter's agreements and
    // never reached the tracker (Amsterdam acceptance, 25 Oct 2026).
    std::unordered_map<std::string, std::size_t> name_holders;
    for (const auto position : candidate_positions) {
      std::unordered_set<std::string> words;
      for (const auto &token : tokenize(request.documents[position].name)) words.insert(token);
      for (const auto &word : words) ++name_holders[word];
    }
    const auto is_named_alone = [&](const RetrievalDocument &document) {
      for (const auto &token : tokenize(document.name)) {
        if (token.size() >= 3 && name_holders[token] == 1 && query_frequency.count(token) > 0) return true;
      }
      return false;
    };
    for (const bool alone : {true, false}) {
      for (const auto &candidate : ranked) {
        if (candidate.document_name_score <= 0.0 ||
            chosen.size() >= request.maximum_chunks) {
          continue;
        }
        if (alone && is_named_alone(*candidate.document) == false) continue;
        if (covered_named_documents.insert(candidate.document->document_id).second) {
          chosen.push_back(&candidate);
          chosen_ids.insert(candidate.chunk->chunk_id);
        }
      }
    }
  }
  // PRODUCT RULE (explicit, live): automatic Matter-wide retrieval must
  // never send document content the user did not actually specify - this
  // fill-up-to-capacity loop had no relevance floor at all in that mode. A
  // query with genuinely zero lexical overlap with any Project document (a
  // bare "…") still had this loop pad the result out to
  // maximum_chunks from `ranked`'s stable tail-order, sending arbitrary,
  // wholly irrelevant document content to the model (and, for a cloud
  // target, through Papinian and off the device) merely because capacity
  // allowed it. `ranked` is sorted strictly non-increasing by score
  // (rank_before), so once a zero/negative score is reached every
  // remaining candidate is too - stopping there is exact, not
  // approximate. Genuine semantic/lexical relevance (score > 0) - not
  // literally naming the document - is what "directly specified in the
  // request" means here: a question actually about a document's real
  // content (e.g. "When may the Buyer terminate under clause 12.3?"
  // against a document whose own text contains that clause) must still
  // find it. Explicit selection is exempt from the floor entirely: a
  // source the user actually selected must always contribute regardless of
  // this one message's own lexical overlap with it (unchanged, pre-
  // existing behavior above).
  // `ranked` is sorted best first, so the floor - like the zero-score stop
  // above it - ends the fill exactly: every later passage scores lower still.
  // Documents picked for the request (SelectedSourceDelivery::RelevanceWithin)
  // are an explicit selection with a floor: each still contributes its best
  // passage, and beyond the window only what the request points at follows.
  const bool bounded_by_relevance = request.relevance_floor_share > 0.0;
  const double relevance_floor = bounded_by_relevance && !ranked.empty()
      ? ranked.front().score * request.relevance_floor_share : 0.0;
  for (const auto &candidate : ranked) {
    if (chosen.size() >= request.maximum_chunks) break;
    if ((!explicit_selection || bounded_by_relevance) && candidate.score <= 0.0) break;
    if (relevance_floor > 0.0 && chosen.size() >= kRelevancePassageWindow && candidate.score < relevance_floor) break;
    if (chosen_ids.insert(candidate.chunk->chunk_id).second) {
      chosen.push_back(&candidate);
    }
  }
  span.add("chosenChunks", chosen.size());
  if (!ranked.empty()) span.add("bestScore", ranked.front().score);

  std::vector<RetrievedContextChunk> result;
  result.reserve(chosen.size());
  for (const auto *candidate : chosen) {
    result.push_back({candidate->document->document_id,
                      candidate->document->name,
                      *candidate->chunk,
                      explicit_selection,
                      candidate->score,
                      candidate->document->title.empty() ? candidate->document->name : candidate->document->title});
  }
  return result;
}

} // namespace singularity::context
