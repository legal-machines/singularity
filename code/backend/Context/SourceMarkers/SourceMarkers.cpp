#include "Context/SourceMarkers/SourceMarkers.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace singularity::context {
namespace {

constexpr const char *kMarkerPrefix = "[SOURCE:";
constexpr std::size_t kMarkerPrefixLength = 8;

std::string marker_key(const std::string &document_token, const std::string &chunk_token) {
  return document_token + ":" + chunk_token;
}

// One "[SOURCE:...]"-shaped span found in some text, located and parsed but
// deliberately not yet judged against any supplied set. The single place
// this repository decides what a marker even looks like: both
// validate_response() (live, against a request's own registry) and
// project_response_references() (later, against that operation's persisted
// evidence) scan through this, so a rendered answer can never disagree with
// the validation that produced it about where a marker is or what it says.
struct MarkerSpan {
  std::size_t start = 0;
  std::size_t length = 0;
  std::string raw_text;
  bool is_well_formed = false;
  std::string document_token;
  std::string chunk_token;
};

std::vector<MarkerSpan> scan_marker_spans(const std::string &text) {
  std::vector<MarkerSpan> spans;
  std::size_t cursor = 0;
  while (true) {
    const auto start = text.find(kMarkerPrefix, cursor);
    if (start == std::string::npos) break;

    const auto close = text.find(']', start);
    if (close == std::string::npos) {
      MarkerSpan span;
      span.start = start;
      span.length = text.size() - start;
      span.raw_text = text.substr(start);
      spans.push_back(std::move(span));
      break;
    }

    MarkerSpan span;
    span.start = start;
    span.length = close - start + 1;
    span.raw_text = text.substr(start, span.length);

    const auto body_start = start + kMarkerPrefixLength;
    const auto body = text.substr(body_start, close - body_start);
    const auto separator = body.find(':');
    // Exactly one non-leading, non-trailing colon is a well-formed
    // "<document_token>:<chunk_token>" body. Anything else (no colon, an
    // empty side, or a second colon - including one belonging to a
    // different, immediately-adjacent marker swallowed by an earlier
    // unterminated one) is malformed rather than guessed at.
    if (separator != std::string::npos && separator != 0 && separator + 1 < body.size() &&
        body.find(':', separator + 1) == std::string::npos) {
      span.is_well_formed = true;
      span.document_token = body.substr(0, separator);
      span.chunk_token = body.substr(separator + 1);
      // Written in a symbol's doubled brackets ("[[SOURCE:DOC_5:CHUNK_1]]", live
      // Luna 26 Sep 2026): the outer pair is the marker's too, never left in
      // the answer around it.
      if (start > 0 && text[start - 1] == '[' && close + 1 < text.size() && text[close + 1] == ']') {
        span.start = start - 1;
        span.length += 2;
        span.raw_text = text.substr(span.start, span.length);
        spans.push_back(std::move(span));
        cursor = close + 2;
        continue;
      }
    } else if (body.find_first_of(";,") != std::string::npos) {
      // Several passages cited in one bracket, as frontier models write them -
      // "[SOURCE:DOC_3:CHUNK_4; SOURCE:DOC_3:CHUNK_1]", "[SOURCE:DOC_1:CHUNK_2,
      // DOC_2:CHUNK_1]" - are those markers each, in order: the first carries
      // the bracket, the rest stand at its end. Only when every part is a
      // well-formed "<document>:<chunk>"; anything else stays one malformed span.
      const auto trimmed = [](std::string value) {
        const auto first = value.find_first_not_of(" \t");
        if (first == std::string::npos) return std::string();
        return value.substr(first, value.find_last_not_of(" \t") - first + 1);
      };
      std::vector<std::pair<std::string, std::string>> parts;
      bool everyPart = true;
      std::size_t from = 0;
      while (everyPart) {
        const std::size_t to = std::min(body.find_first_of(";,", from), body.size());
        std::string part = trimmed(body.substr(from, to - from));
        if (part.rfind("SOURCE:", 0) == 0) part = trimmed(part.substr(7));
        const auto colon = part.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= part.size()
            || part.find(':', colon + 1) != std::string::npos)
          everyPart = false;
        else
          parts.emplace_back(trimmed(part.substr(0, colon)), trimmed(part.substr(colon + 1)));
        if (to >= body.size()) break;
        from = to + 1;
      }
      if (everyPart && parts.size() > 1) {
        span.is_well_formed = true;
        span.document_token = parts.front().first;
        span.chunk_token = parts.front().second;
        spans.push_back(std::move(span));
        for (std::size_t index = 1; index < parts.size(); ++index) {
          MarkerSpan following;
          following.start = close + 1;
          following.is_well_formed = true;
          following.document_token = parts[index].first;
          following.chunk_token = parts[index].second;
          spans.push_back(std::move(following));
        }
        cursor = close + 1;
        continue;
      }
    }

    spans.push_back(std::move(span));
    cursor = close + 1;
  }
  return spans;
}

bool is_inline_space(char value) { return value == ' ' || value == '\t'; }

// Normalises only the whitespace a removed (unresolved) marker leaves
// stranded. It never edits a word, never joins two lines and never removes
// a line break - at most it drops the spaces that existed solely to
// separate prose from a marker that is not being rendered, so an answer
// does not acquire a visible gap where an internal marker used to be.
void tidy_after_removed_marker(std::string &plain, const std::string &text, std::size_t &next_index) {
  std::size_t end = plain.size();
  while (end > 0 && is_inline_space(plain[end - 1])) --end;
  const bool had_space_before = end < plain.size();
  const char next = next_index < text.size() ? text[next_index] : '\0';

  if (had_space_before == true && (next == '\0' || next == '\n' || next == '\r')) {
    plain.erase(end);
    return;
  }
  if (end == 0 || plain[end - 1] == '\n') {
    plain.erase(end);
    while (next_index < text.size() && is_inline_space(text[next_index])) ++next_index;
    return;
  }
  if (had_space_before == true) {
    plain.erase(end + 1);
    while (next_index < text.size() && is_inline_space(text[next_index])) ++next_index;
  }
}

} // namespace

RenderedSourceHandles
render_source_handles_apart(const std::string &text, const std::vector<ResolvedSourceReference> &basis,
                            const std::function<std::string(const ResolvedSourceReference &)> &label_for) {
  constexpr char kOpen = '\x1E';
  constexpr char kClose = '\x1F';
  RenderedSourceHandles rendered = render_source_handles(text, basis, [&](const ResolvedSourceReference &reference) {
    return std::string(1, kOpen) + label_for(reference) + std::string(1, kClose);
  });
  const std::string &marked = rendered.text;
  const auto wordCharacter = [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return std::isalnum(byte) != 0 || byte >= 0x80;
  };
  std::string apart;
  apart.reserve(marked.size() + 32);
  std::size_t position = 0;
  while (position < marked.size()) {
    if (marked[position] != kOpen) {
      apart.push_back(marked[position++]);
      continue;
    }
    std::vector<std::string> run;
    std::size_t cursor = position;
    while (cursor < marked.size() && marked[cursor] == kOpen) {
      const std::size_t close = marked.find(kClose, cursor + 1);
      if (close == std::string::npos) break;
      const std::string label = marked.substr(cursor + 1, close - cursor - 1);
      if (std::find(run.begin(), run.end(), label) == run.end()) run.push_back(label);
      std::size_t next = close + 1;
      while (next < marked.size() && (marked[next] == ' ' || marked[next] == ',' || marked[next] == ';')) ++next;
      cursor = close + 1;
      if (next < marked.size() && marked[next] == kOpen) cursor = next;
      else break;
    }
    if (run.empty()) {
      ++position;
      continue;
    }
    while (!apart.empty() && apart.back() == ' ') apart.pop_back();
    if (!apart.empty() && apart.back() != '\n' && apart.back() != '(') apart.push_back(' ');
    std::string joined;
    for (const std::string &label : run) joined += (joined.empty() ? "" : "; ") + label;
    apart += "(Source: " + joined + ")";
    if (cursor < marked.size() && wordCharacter(marked[cursor])) apart.push_back(' ');
    position = cursor;
  }
  rendered.text = std::move(apart);
  return rendered;
}

RenderedSourceHandles
render_source_handles(const std::string &text, const std::vector<ResolvedSourceReference> &basis,
                      const std::function<std::string(const ResolvedSourceReference &)> &label_for) {
  RenderedSourceHandles rendered;
  std::unordered_set<std::string> seen;
  const auto use = [&](const ResolvedSourceReference &reference, bool documentLevel) {
    if (seen.insert(reference.marker()).second) rendered.resolved.push_back(reference);
    if (!documentLevel) return label_for(reference);
    ResolvedSourceReference documentOnly = reference;
    documentOnly.page.reset();
    documentOnly.heading.reset();
    return label_for(documentOnly);
  };
  const auto byTokens = [&](const std::string &documentToken,
                            const std::optional<std::string> &chunkToken) -> const ResolvedSourceReference * {
    for (const auto &reference : basis)
      if (reference.document_token == documentToken && (!chunkToken || reference.chunk_token == *chunkToken))
        return &reference;
    return nullptr;
  };

  // Pass 1: exact bracketed markers, through the one shared marker scanner.
  std::string afterMarkers;
  std::size_t from = 0;
  for (const MarkerSpan &span : scan_marker_spans(text)) {
    afterMarkers.append(text, from, span.start - from);
    from = span.start + span.length;
    const ResolvedSourceReference *reference =
        span.is_well_formed ? byTokens(span.document_token, span.chunk_token) : nullptr;
    if (reference == nullptr) {
      ++rendered.unresolved_count;
      afterMarkers += kUnverifiedSourceLabel;
    } else {
      afterMarkers += use(*reference, false);
    }
  }
  afterMarkers.append(text, from, std::string::npos);
  for (std::size_t closing; (closing = afterMarkers.find("[/SOURCE]")) != std::string::npos;)
    afterMarkers.erase(closing, 9);
  // A section of Singularity's own request cited as if it were a source - a live
  // GPT-5 answer ended a sentence "[CANONICAL PROJECT CONTEXT entries]" - is
  // internal wording, never shown. Only brackets that open with one of the
  // request's own section names, written in capitals, are taken out.
  static const std::vector<std::string> sectionNames{
      "CANONICAL PROJECT CONTEXT", "PROJECT_CONTEXT", "PROJECT CONTEXT", "SOURCE_MATERIAL", "SOURCE MATERIAL",
      "SOURCE_COVERAGE", "REQUEST_COVERAGE", "CANDIDATE_CONTEXT", "CURRENT_REQUEST", "CONVERSATION_STATE",
      "MATTER_INSTRUCTIONS", "USER_DEFAULT_INSTRUCTIONS", "CAPABILITY",
  };
  std::size_t open = 0;
  while ((open = afterMarkers.find('[', open)) != std::string::npos) {
    const bool namesSection = std::any_of(sectionNames.begin(), sectionNames.end(),
        [&](const std::string &name) { return afterMarkers.compare(open + 1, name.size(), name) == 0; });
    const std::size_t close = namesSection ? afterMarkers.find(']', open) : std::string::npos;
    if (close == std::string::npos) {
      ++open;
      continue;
    }
    std::size_t start = open;
    while (start > 0 && (afterMarkers[start - 1] == ' ' || afterMarkers[start - 1] == '\t')) --start;
    afterMarkers.erase(start, close + 1 - start);
    open = start;
  }

  // Pass 2: paraphrased handles of the same token grammar - with the marker's
  // own "SOURCE:" word when the model dropped only its brackets - and a bare
  // CHUNK_n, which names no document (chunk numbers restart in each one), so is
  // never resolved: it is unverified, and counted.
  static const std::regex handle(
      R"((?:\bSOURCE\s*:\s*)?\bDOC_([0-9]+)(?:\s*[,:;/\-]?\s*(?:CHUNK|Chunk|chunk)[\s_:]*([0-9]+))?\b|\bCHUNK_([0-9]+)\b)",
      std::regex::ECMAScript);
  std::string result;
  std::size_t cursor = 0;
  for (auto match = std::sregex_iterator(afterMarkers.begin(), afterMarkers.end(), handle);
       match != std::sregex_iterator(); ++match) {
    result.append(afterMarkers, cursor, static_cast<std::size_t>(match->position()) - cursor);
    cursor = static_cast<std::size_t>(match->position() + match->length());
    if ((*match)[3].matched) {
      ++rendered.unresolved_count;
      result += kUnverifiedSourceLabel;
      continue;
    }
    const std::string documentToken = "DOC_" + (*match)[1].str();
    const std::optional<std::string> chunkToken = (*match)[2].matched
        ? std::optional<std::string>("CHUNK_" + (*match)[2].str()) : std::nullopt;
    const ResolvedSourceReference *reference = byTokens(documentToken, chunkToken);
    if (reference == nullptr) {
      ++rendered.unresolved_count;
      result += kUnverifiedSourceLabel;
    } else {
      result += use(*reference, !chunkToken.has_value());
    }
  }
  result.append(afterMarkers, cursor, std::string::npos);
  rendered.text = std::move(result);
  return rendered;
}

std::string ResolvedSourceReference::marker() const {
  return "[SOURCE:" + document_token + ":" + chunk_token + "]";
}

std::string format_reference_label(const ResolvedSourceReference &reference) {
  std::string label = reference.document_name;
  if (reference.page) {
    label += ", p. " + std::to_string(*reference.page);
  }
  if (reference.heading && !reference.heading->empty()) {
    label += " - " + *reference.heading;
  }
  return label;
}

std::vector<ResolvedSourceReference>
cited_references(const std::vector<ValidatedSourceReference> &validated) {
  std::vector<ResolvedSourceReference> result;
  std::unordered_set<std::string> seen;
  for (const auto &entry : validated) {
    if (entry.status != SourceReferenceStatus::Supplied || !entry.resolved) {
      continue;
    }
    if (seen.insert(entry.resolved->marker()).second) {
      result.push_back(*entry.resolved);
    }
  }
  return result;
}

SourceMarkerRegistry::SourceMarkerRegistry(const std::vector<RetrievedContextChunk> &chunks) {
  std::unordered_map<std::string, std::size_t> document_numbers;
  std::unordered_map<std::string, std::size_t> next_chunk_number;
  for (const auto &retrieved : chunks) {
    // Defensive: ContextRetriever already deduplicates by chunk_id, but a
    // repeated id here must not silently overwrite an earlier assignment or
    // corrupt the two lookup indexes.
    if (index_by_chunk_id_.count(retrieved.chunk.chunk_id) > 0) {
      continue;
    }

    const auto document_found = document_numbers.find(retrieved.document_id);
    std::size_t document_number;
    if (document_found == document_numbers.end()) {
      document_number = document_numbers.size() + 1;
      document_numbers.emplace(retrieved.document_id, document_number);
    } else {
      document_number = document_found->second;
    }
    const auto chunk_number = ++next_chunk_number[retrieved.document_id];

    ResolvedSourceReference reference;
    reference.document_token = "DOC_" + std::to_string(document_number);
    reference.chunk_token = "CHUNK_" + std::to_string(chunk_number);
    reference.document_id = retrieved.document_id;
    reference.chunk_id = retrieved.chunk.chunk_id;
    reference.document_name = retrieved.document_name;
    reference.page = retrieved.chunk.page;
    reference.heading = retrieved.chunk.heading;
    reference.extractor_version = retrieved.chunk.extractor_version;

    const auto key = marker_key(reference.document_token, reference.chunk_token);
    index_by_chunk_id_.emplace(reference.chunk_id, references_.size());
    index_by_marker_.emplace(key, references_.size());
    references_.push_back(std::move(reference));
  }
}

std::string SourceMarkerRegistry::marker_for(const std::string &chunk_id) const {
  const auto found = index_by_chunk_id_.find(chunk_id);
  if (found == index_by_chunk_id_.end()) {
    throw std::out_of_range(
        "Source marker requested for a chunk this registry was not built from: " + chunk_id);
  }
  return references_[found->second].marker();
}

const std::string &SourceMarkerRegistry::document_token_for(const std::string &chunk_id) const {
  const auto found = index_by_chunk_id_.find(chunk_id);
  if (found == index_by_chunk_id_.end()) {
    throw std::out_of_range(
        "Source document handle requested for a chunk this registry was not built from: " + chunk_id);
  }
  return references_[found->second].document_token;
}

std::string SourceMarkerRegistry::format_chunk(const RetrievedContextChunk &chunk) const {
  return marker_for(chunk.chunk.chunk_id) + "\n" + chunk.chunk.text + "\n[/SOURCE]\n\n";
}

std::string
SourceMarkerRegistry::format_all(const std::vector<RetrievedContextChunk> &chunks) const {
  std::string result;
  for (const auto &chunk : chunks) {
    result += format_chunk(chunk);
  }
  return result;
}

const std::vector<ResolvedSourceReference> &SourceMarkerRegistry::references() const {
  return references_;
}

SourceMarkerRegistry SourceMarkerRegistry::restricted_to(
    const std::unordered_set<std::string> &supplied_chunk_ids) const {
  SourceMarkerRegistry restricted;
  for (const auto &reference : references_) {
    if (supplied_chunk_ids.count(reference.chunk_id) == 0) {
      continue;
    }
    const auto key = marker_key(reference.document_token, reference.chunk_token);
    restricted.index_by_chunk_id_.emplace(reference.chunk_id, restricted.references_.size());
    restricted.index_by_marker_.emplace(key, restricted.references_.size());
    restricted.references_.push_back(reference);
  }
  return restricted;
}

std::vector<ValidatedSourceReference>
SourceMarkerRegistry::validate_response(const std::string &response_text) const {
  std::vector<ValidatedSourceReference> results;
  for (const MarkerSpan &span : scan_marker_spans(response_text)) {
    ValidatedSourceReference entry;
    entry.raw_text = span.raw_text;
    if (span.is_well_formed == false) {
      entry.status = SourceReferenceStatus::Malformed;
      results.push_back(std::move(entry));
      continue;
    }
    const auto found = index_by_marker_.find(marker_key(span.document_token, span.chunk_token));
    if (found == index_by_marker_.end()) {
      entry.status = SourceReferenceStatus::NotSupplied;
    } else {
      entry.status = SourceReferenceStatus::Supplied;
      entry.resolved = references_[found->second];
    }
    results.push_back(std::move(entry));
  }
  return results;
}

namespace {

// A parenthetical that holds nothing but citations - "(see [SOURCE:...])",
// "([SOURCE:...]; [SOURCE:...])", "(cf. [SOURCE:...])" - becomes those
// citations, standing right after the words they support. A citation is
// shown as its own control, so the parenthetical would otherwise be left
// behind empty, as "(see)" - a live GPT-5 answer ended three sentences so.
std::string without_citation_parentheticals(const std::string &text) {
  static const std::regex parenthetical(
      R"(\s*\((?:[Ss]ee(?: also)?|[Cc]f\.|[Ss]ources?:?)?\s*((?:\[SOURCE:[^\]\[]*\]\s*[;,]?\s*)+)\))",
      std::regex::ECMAScript);
  return std::regex_replace(text, parenthetical, "$1");
}

// Where a citation was taken out, before the words around it are closed up: a
// control character no document text holds (every Office writer here refuses
// it, and no answer carries one).
constexpr char kTakenOut = '\x1F';

bool is_citation_separator(char value) { return value == ';' || value == ','; }

// What may follow a citation at the end of a phrase: punctuation, a line's
// end, or the end of the text.
bool ends_a_phrase(char value) {
  static const std::string punctuation = ".,;:!?)]";
  return value == '\0' || value == '\n' || value == '\r' || punctuation.find(value) != std::string::npos;
}

// Whether the words a parenthesis held beside its citations were only the
// words that introduce a citation - "see", "see also", "cf.", "source:".
bool only_citation_words(const std::string &words) {
  std::string folded;
  for (const char character : words) {
    if (is_inline_space(character)) {
      if (!folded.empty() && folded.back() != ' ') folded.push_back(' ');
    } else {
      folded.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
  }
  while (!folded.empty() && folded.back() == ' ') folded.pop_back();
  static const std::vector<std::string> introductions{
      "", "see", "see also", "cf", "cf.", "source", "source:", "sources", "sources:"};
  return std::find(introductions.begin(), introductions.end(), folded) != introductions.end();
}

// Every kTakenOut in `text` removed, with the words around it closed up the way
// a person would have written them without the citation: citations written
// together are one place; the space before a citation that ends a phrase goes,
// and so does a list separator left in front of it; a parenthesis that held
// only citations goes whole; between two words one space remains. Nothing
// else in the text is touched.
std::string close_up_taken_out(const std::string &text) {
  std::string closed;
  closed.reserve(text.size());
  std::size_t index = 0;
  while (index < text.size()) {
    if (text[index] != kTakenOut) {
      closed.push_back(text[index++]);
      continue;
    }
    std::size_t end = index + 1;
    for (std::size_t probe = end;;) {
      while (probe < text.size() && (is_inline_space(text[probe]) || is_citation_separator(text[probe]))) ++probe;
      if (probe >= text.size() || text[probe] != kTakenOut) break;
      end = probe + 1;
      probe = end;
    }
    std::size_t next = end;
    while (next < text.size() && is_inline_space(text[next])) ++next;
    const char following = next < text.size() ? text[next] : '\0';
    std::size_t kept = closed.size();
    while (kept > 0 && is_inline_space(closed[kept - 1])) --kept;
    const bool spaced = kept < closed.size() || next > end;
    if (following == ')' && kept > 0) {
      const std::size_t open = closed.rfind('(', kept - 1);
      const std::size_t lineBreak = closed.rfind('\n', kept - 1);
      if (open != std::string::npos && (lineBreak == std::string::npos || lineBreak < open)
          && only_citation_words(closed.substr(open + 1, kept - open - 1))) {
        std::size_t from = open;
        while (from > 0 && is_inline_space(closed[from - 1])) --from;
        closed.erase(from);
        index = next + 1;
        if (closed.empty() || closed.back() == '\n')
          while (index < text.size() && is_inline_space(text[index])) ++index;
        continue;
      }
    }
    closed.erase(kept);
    if (ends_a_phrase(following)) {
      if (following != '\0' && following != '\n' && following != '\r')
        while (!closed.empty() && is_citation_separator(closed.back())) closed.pop_back();
      while (!closed.empty() && is_inline_space(closed.back())) closed.pop_back();
    } else if (spaced && !closed.empty() && closed.back() != '\n' && closed.back() != '(') {
      closed.push_back(' ');
    }
    index = next;
  }
  return closed;
}

std::string with_unverified_taken_out(std::string text) {
  const std::string label = kUnverifiedSourceLabel;
  for (std::size_t at = text.find(label); at != std::string::npos; at = text.find(label, at + 1))
    text.replace(at, label.size(), 1, kTakenOut);
  return text;
}

} // namespace

ProjectedResponse
project_response_references(const std::string &original_response_text,
                            const std::vector<ResolvedSourceReference> &cited) {
  const std::string response_text = without_citation_parentheticals(original_response_text);
  // Membership here IS the original Supplied verdict: cited_references()
  // only ever admits a reference this exact operation's registry issued and
  // the model actually cited, and MatterTask persists that set verbatim. So
  // a span naming something outside it was fabricated, was never supplied,
  // or is malformed - in every case it is counted, never resolved.
  std::unordered_map<std::string, std::size_t> index_by_marker;
  for (std::size_t index = 0; index < cited.size(); ++index) {
    index_by_marker.emplace(marker_key(cited[index].document_token, cited[index].chunk_token), index);
  }

  ProjectedResponse projected;
  std::vector<int> citation_number_for_reference(cited.size(), 0);
  std::string plain;
  std::vector<std::pair<std::size_t, int>> citation_points;
  std::size_t plain_from = 0;

  for (const MarkerSpan &span : scan_marker_spans(response_text)) {
    if (span.start > plain_from) plain.append(response_text, plain_from, span.start - plain_from);
    plain_from = span.start + span.length;

    const auto found = span.is_well_formed == true
        ? index_by_marker.find(marker_key(span.document_token, span.chunk_token))
        : index_by_marker.end();
    if (found == index_by_marker.end()) {
      ++projected.unresolved_reference_count;
      tidy_after_removed_marker(plain, response_text, plain_from);
      continue;
    }

    const std::size_t reference_index = found->second;
    if (citation_number_for_reference[reference_index] == 0) {
      projected.citations.push_back(cited[reference_index]);
      citation_number_for_reference[reference_index] = static_cast<int>(projected.citations.size());
    }
    citation_points.emplace_back(plain.size(), citation_number_for_reference[reference_index]);
  }
  if (plain_from < response_text.size()) plain.append(response_text, plain_from, std::string::npos);

  std::size_t cursor = 0;
  for (const auto &point : citation_points) {
    if (point.first > cursor) {
      ProjectedResponseSegment segment;
      segment.text = plain.substr(cursor, point.first - cursor);
      projected.segments.push_back(std::move(segment));
      cursor = point.first;
    }
    ProjectedResponseSegment segment;
    segment.citation_number = point.second;
    projected.segments.push_back(std::move(segment));
  }
  if (cursor < plain.size()) {
    ProjectedResponseSegment segment;
    segment.text = plain.substr(cursor);
    projected.segments.push_back(std::move(segment));
  }
  // A paraphrased handle of the same token grammar outside any marker
  // ("DOC_2", "DOC_2, chunk 3", a stray "[/SOURCE]" or "CHUNK_3") is internal
  // syntax too: every text segment is rendered against the same cited set by
  // the rule retained artifacts use (render_source_handles()), never shown to
  // the user as a handle.
  for (ProjectedResponseSegment &segment : projected.segments) {
    if (segment.citation_number != 0) continue;
    RenderedSourceHandles rendered = render_source_handles(segment.text, cited, format_reference_label);
    segment.text = std::move(rendered.text);
    projected.unresolved_reference_count += rendered.unresolved_count;
  }
  return projected;
}

RenderedSourceHandles strip_source_handles(const std::string &text,
                                           const std::vector<ResolvedSourceReference> &basis) {
  // "(see [SOURCE:...])" is a citation too: it goes whole with its marker.
  const std::string cited = text.find(kMarkerPrefix) == std::string::npos ? text
      : without_citation_parentheticals(text);
  RenderedSourceHandles stripped = render_source_handles(cited, basis,
      [](const ResolvedSourceReference &) { return std::string(1, kTakenOut); });
  stripped.text = close_up_taken_out(with_unverified_taken_out(std::move(stripped.text)));
  return stripped;
}

std::string without_unverified_labels(const std::string &text) {
  if (text.find(kUnverifiedSourceLabel) == std::string::npos) return text;
  return close_up_taken_out(with_unverified_taken_out(text));
}

} // namespace singularity::context
