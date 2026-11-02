#include "PapinianGuard/DeterministicDetector/DeterministicDetector.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <regex>
#include <string_view>
#include <utility>

namespace singularity::protected_api {
namespace {

bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

// `group` 0 is the whole match; a pattern that needs a keyword to be sure
// ("SWIFT: CHASUS33") masks only the value it captured, so the keyword that
// says what the value is stays readable.
std::vector<Match> find_regex_matches(const std::string &text, const std::regex &pattern,
                                      const std::string &label, std::size_t group = 0) {
  std::vector<Match> matches;
  auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
  const auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    const auto &match = *it;
    if (!match[group].matched) continue;
    matches.push_back(Match{static_cast<std::size_t>(match.position(group)),
                            static_cast<std::size_t>(match.length(group)), label, match.str(group)});
  }
  return matches;
}

// $, £, €, ¥ followed (optionally through one space) by a numeral. The
// non-ASCII symbols are matched as their literal UTF-8 byte sequences
// rather than through std::regex, which has no reliable notion of a
// multi-byte "character" in a bracket expression.
std::vector<Match> find_money_symbol_matches(const std::string &text) {
  static const std::vector<std::string> markers{"$", "\xC2\xA3", "\xE2\x82\xAC", "\xC2\xA5"};

  std::vector<Match> matches;
  for (const auto &marker : markers) {
    std::size_t search_from = 0;
    std::size_t marker_pos;
    while ((marker_pos = text.find(marker, search_from)) != std::string::npos) {
      // "$B$3", "Параметры!$B$3": the anchor before a row number in a formula's
      // absolute reference, not an amount - a column's letters stand between it
      // and another anchor or a sheet name. "US$3" is still money.
      if (marker == "$") {
        std::size_t letters = marker_pos;
        while (letters > 0 && marker_pos - letters < 3 && text[letters - 1] >= 'A' && text[letters - 1] <= 'Z') --letters;
        if (letters < marker_pos && letters > 0 && (text[letters - 1] == '$' || text[letters - 1] == '!')) {
          search_from = marker_pos + marker.size();
          continue;
        }
      }
      std::size_t cursor = marker_pos + marker.size();
      if (cursor < text.size() && text[cursor] == ' ') {
        cursor++;
      }
      const std::size_t number_start = cursor;
      bool saw_digit = false;
      while (cursor < text.size() &&
             (is_ascii_digit(text[cursor]) || text[cursor] == ',' || text[cursor] == '.')) {
        if (is_ascii_digit(text[cursor])) saw_digit = true;
        cursor++;
      }
      // A comma or a full stop after the figures is the sentence's, not the
      // amount's: "to $1,540,800, reflecting" (a live answer restored
      // "$1,540,800,," once the model wrote its own comma after the symbol).
      while (cursor > number_start && (text[cursor - 1] == ',' || text[cursor - 1] == '.')) cursor--;
      // "Amount ($000)": a column counted in thousands of dollars, no amount.
      const bool scaleMark = text.compare(number_start, cursor - number_start, "000") == 0;
      if (saw_digit && cursor > number_start && !scaleMark) {
        matches.push_back(Match{marker_pos, cursor - marker_pos, to_string(Category::MonetaryAmounts),
                                text.substr(marker_pos, cursor - marker_pos)});
      }
      search_from = marker_pos + marker.size();
    }
  }
  return matches;
}

std::vector<Match> find_money_matches(const std::string &text) {
  // A code and its figures stand on one line and in one cell, a space apart at
  // most: "Amount, USD\n1\tRent" is a table's header and its first row's number,
  // which "\s" once joined into one amount "USD\n1" (a verifier's run, 25 Sep 2026).
  static const std::regex code_pattern(
      R"(\b(?:(?:USD|EUR|GBP|CHF|JPY|CAD|AUD) ?\d[\d,]*(?:\.\d+)?|\d[\d,]*(?:\.\d+)? ?(?:USD|EUR|GBP|CHF|JPY|CAD|AUD))\b)");

  auto matches = find_money_symbol_matches(text);
  auto code_matches = find_regex_matches(text, code_pattern, to_string(Category::MonetaryAmounts));
  matches.insert(matches.end(), code_matches.begin(), code_matches.end());
  return matches;
}

std::vector<Match> find_url_matches(const std::string &text) {
  static const std::regex pattern(R"(https?://[^\s<>"']+)");
  static const std::string trailing_punctuation = ".,;:!?)]}'\"";

  auto matches = find_regex_matches(text, pattern, to_string(Category::Urls));
  for (auto &match : matches) {
    while (match.length > 0 &&
           trailing_punctuation.find(match.value.back()) != std::string::npos) {
      match.value.pop_back();
      match.length--;
    }
  }
  return matches;
}

// CodePoint helpers are defined below; the reference-style identifier in any
// script is found with them.
std::vector<Match> find_code_identifier_matches(const std::string &text);

// Where a phone's extension written at text[at] ends - "ext. 214", "x0188",
// «доб. 123», "extension 4410" - or `at` when none is written there. A bare
// "x" is an extension only where `bare_x` says the text allows one: after a
// phone, or inside brackets ("Controller (x0188)"); "2 x 100" is arithmetic.
std::size_t phone_extension_end(const std::string &text, std::size_t at, bool bare_x) {
  static const std::string_view words[] = {"extension", "ext.", "ext", "\xD0\xB4\xD0\xBE\xD0\xB1\xD0\xB0\xD0\xB2"
      "\xD0\xBE\xD1\x87\xD0\xBD\xD1\x8B\xD0\xB9", "\xD0\xB4\xD0\xBE\xD0\xB1.", "\xD0\xB4\xD0\xBE\xD0\xB1"};
  std::size_t cursor = at;
  std::size_t keyword = 0;
  for (const std::string_view word : words) {
    if (text.size() - cursor < word.size()) continue;
    bool same = true;
    for (std::size_t index = 0; index < word.size() && same; ++index)
      same = std::tolower(static_cast<unsigned char>(text[cursor + index])) == static_cast<unsigned char>(word[index]);
    if (same) {
      keyword = word.size();
      break;
    }
  }
  if (keyword == 0 && bare_x && cursor < text.size() && (text[cursor] == 'x' || text[cursor] == 'X')) keyword = 1;
  if (keyword == 0) return at;
  cursor += keyword;
  if (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == ':')) ++cursor;
  if (cursor < text.size() && text[cursor] == ' ') ++cursor;
  std::size_t digits = 0;
  while (cursor + digits < text.size() && is_ascii_digit(text[cursor + digits])) ++digits;
  if (digits < 2 || digits > 6) return at;
  const std::size_t end = cursor + digits;
  if (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) != 0
          || static_cast<unsigned char>(text[end]) >= 0x80))
    return at;
  return end;
}

// Phones: an international number with its extension, and an extension
// written alone - "Controller (x0188)", "ext. 214", «доб. 123» - which dials
// one person as surely as the number does.
std::vector<Match> find_phone_matches(const std::string &text, const std::regex &pattern, const std::string &label) {
  std::vector<Match> matches = find_regex_matches(text, pattern, label);
  for (Match &match : matches) {
    std::size_t cursor = match.start + match.length;
    while (cursor < text.size() && cursor - (match.start + match.length) < 2 && (text[cursor] == ' ' || text[cursor] == ','))
      ++cursor;
    const std::size_t end = phone_extension_end(text, cursor, true);
    if (end > cursor) {
      match.length = end - match.start;
      match.value = text.substr(match.start, match.length);
    }
  }
  for (std::size_t position = 0; position < text.size(); ++position) {
    const unsigned char character = static_cast<unsigned char>(text[position]);
    if (character != 'x' && character != 'X' && character != 'e' && character != 'E' && character != 0xD0) continue;
    if (position > 0 && (std::isalnum(static_cast<unsigned char>(text[position - 1])) != 0
            || static_cast<unsigned char>(text[position - 1]) >= 0x80))
      continue;
    const bool bracketed = position > 0 && text[position - 1] == '(';
    const std::size_t end = phone_extension_end(text, position, bracketed);
    if (end == position) continue;
    if (bracketed && (end >= text.size() || text[end] != ')')) continue;
    const bool inside = std::any_of(matches.begin(), matches.end(), [&](const Match &match) {
      return position >= match.start && position < match.start + match.length;
    });
    if (!inside) matches.push_back(Match{position, end - position, label, text.substr(position, end - position)});
    position = end;
  }
  std::sort(matches.begin(), matches.end(), [](const Match &left, const Match &right) { return left.start < right.start; });
  return matches;
}

// ---- Domestic phones and digit-group identifiers ------------------------------------------------
//
// The shapes a US or a Russian document writes its own numbers in, which the
// international "+1 ..." / "+7 ..." shape misses: "(201) 555-0132",
// "201-555-0132", "8 (495) 555-31-08"; an SSN "123-45-6789", an EIN
// "93-2748816", a card "4111 1111 1111 1111", an IBAN, a СНИЛС "123-456-789
// 00", a register number "7702-418856-03". Each is found whole, so no part of
// it is left readable beside a mask.

// The line text[position] stands on, lower case (ASCII and Cyrillic).
std::string lowered_line_at(const std::string &text, std::size_t position) {
  std::size_t start = position;
  while (start > 0 && text[start - 1] != '\n') --start;
  std::size_t end = position;
  while (end < text.size() && text[end] != '\n') ++end;
  std::string line = text.substr(start, end - start);
  for (std::size_t index = 0; index < line.size(); ++index) {
    const unsigned char byte = static_cast<unsigned char>(line[index]);
    if (byte >= 'A' && byte <= 'Z') line[index] = static_cast<char>(byte + 32);
    else if (byte == 0xD0 && index + 1 < line.size()) {
      const unsigned char next = static_cast<unsigned char>(line[index + 1]);
      if (next >= 0x90 && next <= 0x9F) line[index + 1] = static_cast<char>(next + 0x20);
      else if (next >= 0xA0 && next <= 0xAF) { line[index] = static_cast<char>(0xD1); line[index + 1] = static_cast<char>(next - 0x20); }
      ++index;
    }
  }
  return line;
}

// Whether the line a number stands on, or the one before it, says it is a
// phone: "Tel.", "Phone:", "Fax", "Mobile", "T:", «тел.», «факс», «моб.» - or
// writes an e-mail address beside it, as a letterhead or a signature block does.
bool phone_context(const std::string &text, std::size_t position) {
  static const std::string_view cues[] = {"tel", "tel.", "telephone", "phone", "phones", "fax", "mobile", "cell", "mob",
      "ph", "t", "p", "f", "m", "o", "direct", "office", "call", "text", "whatsapp",
      "\xD1\x82\xD0\xB5\xD0\xBB", "\xD1\x82\xD0\xB5\xD0\xBB\xD0\xB5\xD1\x84\xD0\xBE\xD0\xBD", "\xD1\x84\xD0\xB0\xD0\xBA\xD1\x81",
      "\xD0\xBC\xD0\xBE\xD0\xB1", "\xD0\xBC\xD0\xBE\xD0\xB1\xD0\xB8\xD0\xBB\xD1\x8C\xD0\xBD\xD1\x8B\xD0\xB9", "\xD1\x81\xD0\xBE\xD1\x82"};
  const auto cued = [&](const std::string &line, bool whole) {
    if (line.find('@') != std::string::npos) return true;
    std::size_t index = 0;
    while (index < line.size()) {
      const unsigned char byte = static_cast<unsigned char>(line[index]);
      const bool letter = std::isalpha(byte) != 0 || byte == 0xD0 || byte == 0xD1;
      if (!letter) { ++index; continue; }
      std::size_t end = index;
      while (end < line.size() && (std::isalpha(static_cast<unsigned char>(line[end])) != 0
                                   || static_cast<unsigned char>(line[end]) >= 0x80))
        ++end;
      const std::string word = line.substr(index, end - index);
      // One letter counts only as a label: "T: 201-555-0132", "M. 8 916 ...".
      const bool labelled = end < line.size() && (line[end] == ':' || line[end] == '.');
      for (const std::string_view cue : cues)
        if (word == cue && (cue.size() > 1 || labelled) && (whole || cue.size() > 1)) return true;
      index = end;
    }
    return false;
  };
  if (cued(lowered_line_at(text, position), true)) return true;
  std::size_t start = position;
  while (start > 0 && text[start - 1] != '\n') --start;
  if (start > 1 && cued(lowered_line_at(text, start - 2), false)) return true;
  // A signature block writes the e-mail on the line after the number:
  // "Jane Roe\nPartner\n212.555.0199\njroe@example.com".
  std::size_t end = position;
  while (end < text.size() && text[end] != '\n') ++end;
  return end + 1 < text.size() && cued(lowered_line_at(text, end + 1), false);
}

std::vector<Match> find_domestic_phone_matches(const std::string &text, const std::string &label) {
  // North American: "(201) 555-0132", "201-555-0132", "201.555.0132", "1-800-555-0132".
  static const std::regex nanp(
      R"((?:\b1[ .\-])?(\(\d{3}\)\s?|\b\d{3}[ .\-])\d{3}[ .\-]\d{4}\b)");
  // Russian: "8 (495) 555-31-08", "8-495-555-31-08", "(495) 555-31-08", "8 916 555 31 08".
  static const std::regex russian(
      R"((?:\b8[ \-]?)?(\(\d{3,5}\)\s?|\b\d{3}[ \-])\d{1,3}[ \-]?\d{2}[ \-]\d{2}\b)");
  // Russian, the subscriber's seven figures run together: "8 916 1234567", "8 (916) 1234567".
  static const std::regex russianRun(R"(\b8[ \-]?(\(\d{3}\)\s?|\d{3}[ \-]?)\d{7}\b)");
  std::vector<Match> matches;
  for (const std::regex *pattern : {&nanp, &russian, &russianRun}) {
    for (std::sregex_iterator it(text.begin(), text.end(), *pattern), end; it != end; ++it) {
      const std::size_t start = static_cast<std::size_t>(it->position(0));
      const std::size_t length = static_cast<std::size_t>(it->length(0));
      if (start > 0 && (text[start - 1] == '+' || is_ascii_digit(text[start - 1]) || text[start - 1] == '-'
                        || text[start - 1] == '.' || text[start - 1] == '/'))
        continue;
      const std::size_t stop = start + length;
      if (stop < text.size() && (is_ascii_digit(text[stop]) || text[stop] == '-' || text[stop] == '/'
                                 || (text[stop] == '.' && stop + 1 < text.size() && is_ascii_digit(text[stop + 1]))))
        continue;
      // A bracketed area code is a phone's own writing, and so is a North
      // American number written with one mark throughout and valid as one
      // ("212.555.0199", "212-555-0199": neither its area code nor its exchange
      // begins with 0 or 1), and a Russian "8 916 1234567". The others need a
      // word or a line that says phone.
      const std::string written = it->str();
      const bool bracketed = (*it)[1].matched && (*it)[1].str().front() == '(';
      bool nanpShape = false;
      if (pattern == &nanp && !bracketed) {
        std::string digits;
        std::string marks;
        for (const char character : written) {
          if (is_ascii_digit(character)) digits.push_back(character);
          else marks.push_back(character);
        }
        const std::string local = digits.size() == 11 ? digits.substr(1) : digits;
        nanpShape = local.size() == 10 && local[0] >= '2' && local[3] >= '2' && !marks.empty()
            && marks.find_first_not_of(marks.back()) == std::string::npos && (marks.back() == '.' || marks.back() == '-');
      }
      if (!bracketed && !nanpShape && pattern != &russianRun && !phone_context(text, start)) continue;
      const bool inside = std::any_of(matches.begin(), matches.end(), [&](const Match &match) {
        return start < match.start + match.length && match.start < stop;
      });
      if (!inside) matches.push_back(Match{start, length, label, text.substr(start, length)});
    }
  }
  return matches;
}

// Luhn's check, which every payment card number passes.
bool luhn_valid(const std::string &digits) {
  int sum = 0;
  bool twice = false;
  for (std::size_t index = digits.size(); index-- > 0;) {
    int digit = digits[index] - '0';
    if (twice) {
      digit *= 2;
      if (digit > 9) digit -= 9;
    }
    sum += digit;
    twice = !twice;
  }
  return digits.size() >= 13 && sum % 10 == 0;
}

// ISO 13616's check, which every IBAN passes.
bool iban_valid(const std::string &compact) {
  if (compact.size() < 15 || compact.size() > 34) return false;
  const std::string moved = compact.substr(4) + compact.substr(0, 4);
  int remainder = 0;
  for (const char character : moved) {
    if (is_ascii_digit(character)) remainder = (remainder * 10 + (character - '0')) % 97;
    else if (character >= 'A' && character <= 'Z') remainder = (remainder * 100 + (character - 'A' + 10)) % 97;
    else return false;
  }
  return remainder == 1;
}

std::vector<Match> find_digit_group_identifiers(const std::string &text, const std::string &label) {
  std::vector<Match> matches;
  const auto add = [&](std::size_t start, std::size_t end) {
    const bool inside = std::any_of(matches.begin(), matches.end(), [&](const Match &match) {
      return start < match.start + match.length && match.start < end;
    });
    if (!inside) matches.push_back(Match{start, end - start, label, text.substr(start, end - start)});
  };
  const auto digits_of = [](const std::string &written) {
    std::string digits;
    for (const char character : written)
      if (is_ascii_digit(character)) digits.push_back(character);
    return digits;
  };
  // A payment card: four groups of four (or 4-6-5), Luhn-valid.
  static const std::regex card(R"(\b(?:\d{4}[ \-]\d{4}[ \-]\d{4}[ \-]\d{4}(?:[ \-]\d{1,3})?|\d{4}[ \-]\d{6}[ \-]\d{5})\b)");
  for (std::sregex_iterator it(text.begin(), text.end(), card), end; it != end; ++it)
    if (luhn_valid(digits_of(it->str()))) add(static_cast<std::size_t>(it->position()), static_cast<std::size_t>(it->position() + it->length()));
  // An IBAN, written whole or in groups of four, with its check.
  static const std::regex iban(R"(\b[A-Z]{2}\d{2}(?: ?[A-Z0-9]{4}){2,7}(?: ?[A-Z0-9]{1,4})?\b)");
  for (std::sregex_iterator it(text.begin(), text.end(), iban), end; it != end; ++it) {
    std::string compact;
    for (const char character : it->str())
      if (character != ' ') compact.push_back(character);
    if (iban_valid(compact)) add(static_cast<std::size_t>(it->position()), static_cast<std::size_t>(it->position() + it->length()));
  }
  // A СНИЛС: "123-456-789 00".
  static const std::regex snils(R"(\b\d{3}-\d{3}-\d{3}[ \-]\d{2}\b)");
  for (std::sregex_iterator it(text.begin(), text.end(), snils), end; it != end; ++it)
    add(static_cast<std::size_t>(it->position()), static_cast<std::size_t>(it->position() + it->length()));
  // Groups of figures joined by hyphens, nine figures or more in all: an SSN
  // "123-45-6789", an EIN "93-2748816", "7702-418856-03". A date or a range
  // of two small numbers has fewer.
  static const std::regex groups(R"(\b\d{1,8}(?:-\d{1,8}){1,5}\b)");
  for (std::sregex_iterator it(text.begin(), text.end(), groups), end; it != end; ++it) {
    const std::size_t start = static_cast<std::size_t>(it->position());
    const std::size_t stop = start + static_cast<std::size_t>(it->length());
    // Never a part of a longer writing: "+1-", "No.-", a decimal, a code's letters.
    if (start > 0 && (text[start - 1] == '+' || text[start - 1] == '.' || text[start - 1] == ',' || text[start - 1] == '/'
                      || text[start - 1] == '-' || std::isalpha(static_cast<unsigned char>(text[start - 1])) != 0))
      continue;
    if (stop < text.size() && (text[stop] == '/' || text[stop] == '-'
                               || ((text[stop] == '.' || text[stop] == ',') && stop + 1 < text.size() && is_ascii_digit(text[stop + 1]))))
      continue;
    if (digits_of(it->str()).size() < 9) continue;
    // A chain that opens with a day ("2026-03-03-001") is the date reading's to judge.
    static const std::regex dated(R"(^\d{4}-\d{2}-\d{2}(?:-|$))");
    if (std::regex_search(it->str(), dated)) continue;
    add(start, stop);
  }
  return matches;
}

std::vector<Match> find_category_matches(const std::string &text, Category category) {
  static const std::regex email_pattern(R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})");
  // International format (leading '+'); the domestic US and Russian shapes are
  // found beside it where a phone's writing or its line says phone
  // (find_domestic_phone_matches).
  // An area code in brackets is part of it: "+1 (212) 555-0199", "+7(495)1234567".
  static const std::regex phone_pattern(
      R"(\+\d{1,3}(?:[ .\-]?\(\d{1,5}\)(?:[ .\-]?\d{2,7}){1,4}|(?:[ .\-]?\d{2,4}){2,5}))");
  // Reference-style identifiers (e.g. "A/2024/00931", "REF-2024-0099") or a
  // bare run of 6+ digits (account/registration numbers).
  static const std::regex identifier_pattern(
      R"(\b(?:[A-Za-z]{1,4}[/\-]\d{2,6}[/\-]\d{2,6}(?:[/\-]\d{1,6})?|\d{6,20})\b)");
  // A SWIFT/BIC code in wire instructions ("SWIFT COBADEFFXXX", "BIC:
  // CHASUS33", "SWIFT-код: SABRRUMM"): 8 or 11 capitals and digits, which no
  // other identifier shape covers, while the Russian БИК is a run of digits
  // the pattern above already masks. Only after the word SWIFT or BIC, and
  // only in capitals, so "a SWIFT transfer" keeps its ordinary words.
  static const std::regex swift_pattern(
      R"(\b(?:SWIFT|Swift|swift|BIC|bic)(?:\s*/\s*(?:SWIFT|BIC))?(?:\s*-?\s*(?:code|Code|CODE|код))?)"
      R"(\s*(?:\((?:SWIFT|BIC)\))?\s*[:#\-]?\s*([A-Z]{6}[A-Z0-9]{2}(?:[A-Z0-9]{3})?)\b)");

  switch (category) {
    case Category::MonetaryAmounts:
      return find_money_matches(text);
    case Category::Emails:
      return find_regex_matches(text, email_pattern, to_string(category));
    case Category::PhoneNumbers: {
      std::vector<Match> matches = find_phone_matches(text, phone_pattern, to_string(category));
      for (Match &domestic : find_domestic_phone_matches(text, to_string(category))) {
        const bool inside = std::any_of(matches.begin(), matches.end(), [&](const Match &match) {
          return domestic.start < match.start + match.length && match.start < domestic.start + domestic.length;
        });
        if (!inside) matches.push_back(std::move(domestic));
      }
      return matches;
    }
    case Category::Urls:
      return find_url_matches(text);
    case Category::Identifiers: {
      auto matches = find_regex_matches(text, identifier_pattern, to_string(category));
      auto swift = find_regex_matches(text, swift_pattern, to_string(category), 1);
      matches.insert(matches.end(), swift.begin(), swift.end());
      auto codes = find_code_identifier_matches(text);
      matches.insert(matches.end(), codes.begin(), codes.end());
      auto groups = find_digit_group_identifiers(text, to_string(category));
      matches.insert(matches.end(), groups.begin(), groups.end());
      return matches;
    }
  }
  return {};
}

// One code point of `text`, decoded and classified by utf8proc. A length of
// 0 means the end of the text or bytes that are not UTF-8.
struct CodePoint {
  std::size_t length = 0;
  utf8proc_int32_t value = -1;
  utf8proc_category_t category = UTF8PROC_CATEGORY_CN;
};

CodePoint code_point_at(const std::string &text, std::size_t position) {
  if (position >= text.size()) return {};
  utf8proc_int32_t value = -1;
  const auto read = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data() + position),
                                     static_cast<utf8proc_ssize_t>(text.size() - position), &value);
  if (read <= 0) return {};
  return CodePoint{static_cast<std::size_t>(read), value, utf8proc_category(value)};
}

bool is_upper(const CodePoint &point) {
  return point.category == UTF8PROC_CATEGORY_LU || point.category == UTF8PROC_CATEGORY_LT;
}

bool is_lower(const CodePoint &point) { return point.category == UTF8PROC_CATEGORY_LL; }

bool is_digit(const CodePoint &point) { return point.category == UTF8PROC_CATEGORY_ND; }

// A letter, a combining mark or a number: anything that continues a word.
bool is_word(const CodePoint &point) {
  switch (point.category) {
    case UTF8PROC_CATEGORY_LU:
    case UTF8PROC_CATEGORY_LL:
    case UTF8PROC_CATEGORY_LT:
    case UTF8PROC_CATEGORY_LM:
    case UTF8PROC_CATEGORY_LO:
    case UTF8PROC_CATEGORY_MN:
    case UTF8PROC_CATEGORY_MC:
    case UTF8PROC_CATEGORY_ME:
    case UTF8PROC_CATEGORY_ND:
    case UTF8PROC_CATEGORY_NL:
    case UTF8PROC_CATEGORY_NO:
      return point.length > 0;
    default:
      return false;
  }
}

// Spaces within one line, the no-break space of Russian legal text included.
// A line break ends an address or a name.
std::size_t skip_spaces(const std::string &text, std::size_t position) {
  for (;;) {
    const CodePoint point = code_point_at(text, position);
    if (point.length == 0 || !(point.category == UTF8PROC_CATEGORY_ZS || point.value == '\t')) return position;
    position += point.length;
  }
}

bool starts_with_at(const std::string &text, std::size_t position, std::string_view literal) {
  return text.compare(position, literal.size(), literal) == 0;
}

// Letters, digits and inner hyphens from `position`: "Тверская-Ямская", "1-я",
// "12А", and with `slash` also "5/2".
std::size_t word_run_end(const std::string &text, std::size_t position, bool slash) {
  for (;;) {
    const CodePoint point = code_point_at(text, position);
    if (is_word(point)) {
      position += point.length;
      continue;
    }
    if (point.value == '-' || (slash && point.value == '/')) {
      const CodePoint after = code_point_at(text, position + 1);
      if (is_word(after)) {
        position += 1 + after.length;
        continue;
      }
    }
    return position;
  }
}

// Calls `found(start)` at every position that begins a word - the text's
// start, or a code point that follows no letter or digit - and records the
// span it returns. A marker is only a marker as a whole word: "наш." does
// not hold "ш.", and "ТИП" does not hold "ИП".
template <typename Find>
std::vector<Match> find_at_word_starts(const std::string &text, const char *label, Find found) {
  std::vector<Match> matches;
  bool afterWord = false;
  std::size_t position = 0;
  while (position < text.size()) {
    const CodePoint point = code_point_at(text, position);
    if (point.length == 0) {
      afterWord = false;
      ++position;
      continue;
    }
    if (!afterWord) {
      if (const std::optional<std::pair<std::size_t, std::size_t>> span = found(position)) {
        matches.push_back(Match{span->first, span->second - span->first, label,
                                text.substr(span->first, span->second - span->first)});
        position = span->second;
        afterWord = true;
        continue;
      }
    }
    afterWord = is_word(point);
    position += point.length;
  }
  return matches;
}

// One word of a street's name: "Садовая", "8", "1-я", "Тверская-Ямская", an
// abbreviated "Б." or the connector "им.". A name needs one capitalised word,
// so "пл. 50 кв. м" (an area) is not a street.
std::optional<std::size_t> street_word_end(const std::string &text, std::size_t position,
                                           bool &capitalised) {
  static constexpr std::string_view connector = "им.";
  if (starts_with_at(text, position, connector)) return position + connector.size();
  const CodePoint first = code_point_at(text, position);
  if (!is_upper(first) && !is_digit(first)) return std::nullopt;
  if (is_upper(first)) capitalised = true;
  const std::size_t end = word_run_end(text, position + first.length, false);
  if (end == position + first.length && is_upper(first) && end < text.size() && text[end] == '.') return end + 1;
  return end;
}

// The house, building, flat or office after a street: ", д. 5", " корп. 2",
// ", кв. 12", "стр. 1А", "лит. Б". Its number starts with a digit or a capital.
std::optional<std::size_t> house_part_end(const std::string &text, std::size_t position) {
  static constexpr std::string_view markers[] = {
      "д.", "дом", "корп.", "корпус", "к.", "стр.", "строение", "кв.", "квартира",
      "оф.", "офис", "пом.", "помещение", "лит.", "литера",
  };
  std::size_t cursor = skip_spaces(text, position);
  if (cursor < text.size() && text[cursor] == ',') cursor = skip_spaces(text, cursor + 1);
  if (cursor == position) return std::nullopt;
  for (const std::string_view marker : markers) {
    if (!starts_with_at(text, cursor, marker)) continue;
    std::size_t valueStart = cursor + marker.size();
    // A whole word must end where it is written: "дома" is not "дом".
    if (marker.back() != '.' && is_word(code_point_at(text, valueStart))) continue;
    valueStart = skip_spaces(text, valueStart);
    const CodePoint first = code_point_at(text, valueStart);
    if (!is_digit(first) && !is_upper(first)) continue;
    return word_run_end(text, valueStart + first.length, true);
  }
  return std::nullopt;
}

// A Russian street address as it is usually written: a street marker, the
// street's name, then the house, building and flat - "ул. Садовая, д. 5, кв.
// 12". The semantic model finds the city in such an address but sends the
// street and the flat in clear, and nothing else here knew their shape. The
// whole run is one value, marker included; the postcode is already masked as
// a number, and the city by the model. "пр." (also "приказ"), "пл." (also an
// area) and "проспект" (also a securities prospectus) count only with a house
// number after them.
std::optional<std::pair<std::size_t, std::size_t>> russian_address_at(const std::string &text,
                                                                       std::size_t start) {
  struct StreetMarker {
    std::string_view text;
    bool needs_house;
  };
  static constexpr StreetMarker markers[] = {
      {"ул.", false},      {"Ул.", false},      {"улица", false},    {"Улица", false},
      {"пр-т", false},     {"пр-кт", false},    {"просп.", false},   {"проспект", true},
      {"Проспект", true},  {"пр.", true},       {"пер.", false},     {"переулок", false},
      {"Переулок", false}, {"ш.", false},       {"шоссе", false},    {"б-р", false},
      {"бульвар", false},  {"Бульвар", false},  {"наб.", false},     {"набережная", false},
      {"Набережная", false}, {"пл.", true},
  };
  for (const StreetMarker &marker : markers) {
    if (!starts_with_at(text, start, marker.text)) continue;
    std::size_t cursor = start + marker.text.size();
    if (marker.text.back() != '.' && is_word(code_point_at(text, cursor))) continue;
    cursor = skip_spaces(text, cursor);
    bool capitalised = false;
    std::optional<std::size_t> streetEnd;
    for (int words = 0; words < 6; ++words) {
      const auto wordEnd = street_word_end(text, cursor, capitalised);
      if (!wordEnd) break;
      streetEnd = *wordEnd;
      const std::size_t next = skip_spaces(text, *wordEnd);
      if (next == *wordEnd) break;
      cursor = next;
    }
    if (!streetEnd || !capitalised) continue;
    std::size_t end = *streetEnd;
    bool house = false;
    while (const auto partEnd = house_part_end(text, end)) {
      end = *partEnd;
      house = true;
    }
    if (marker.needs_house && !house) continue;
    return std::make_pair(start, end);
  }
  return std::nullopt;
}

// The same address with the street's name before its kind, as a document
// often writes it: "Невский проспект, дом 28", "Тверская улица, д. 7",
// "Ленинградское шоссе, д. 39, стр. 1". Only with a house number after it:
// "Невский проспект" alone is a street anyone may name.
std::optional<std::pair<std::size_t, std::size_t>> russian_named_street_at(const std::string &text,
                                                                           std::size_t start) {
  static constexpr std::string_view kinds[] = {
      "проспект", "улица", "переулок", "шоссе", "бульвар", "набережная", "площадь", "проезд",
      "пр-т", "пр-кт", "просп.", "пр.", "ул.", "пер.", "ш.", "б-р", "наб.", "пл.",
  };
  bool capitalised = false;
  std::size_t cursor = start;
  for (int words = 0; words < 2; ++words) {
    const auto wordEnd = street_word_end(text, cursor, capitalised);
    if (!wordEnd) return std::nullopt;
    const std::size_t next = skip_spaces(text, *wordEnd);
    if (next == *wordEnd) return std::nullopt;
    for (const std::string_view kind : kinds) {
      if (!starts_with_at(text, next, kind)) continue;
      if (kind.back() != '.' && is_word(code_point_at(text, next + kind.size()))) continue;
      std::size_t end = next + kind.size();
      bool house = false;
      while (const auto partEnd = house_part_end(text, end)) {
        end = *partEnd;
        house = true;
      }
      if (!house || !capitalised) return std::nullopt;
      return std::make_pair(start, end);
    }
    cursor = next;
  }
  return std::nullopt;
}

bool is_ascii_upper(char c) { return c >= 'A' && c <= 'Z'; }

bool is_ascii_letter(char c) { return is_ascii_upper(c) || (c >= 'a' && c <= 'z'); }

// One word of an English street's name, without its full stop: "Main",
// "Fifth", "O'Hare", "Wilkes-Barre", or an ordinal - "42nd", "5th". A plain
// number or a word in lowercase is never part of a street, so "12 months" and
// "Section 12.4 of" name none.
std::optional<std::size_t> english_street_word_end(const std::string &text, std::size_t position) {
  if (position >= text.size()) return std::nullopt;
  std::size_t cursor = position;
  if (is_ascii_digit(text[cursor])) {
    while (cursor < text.size() && is_ascii_digit(text[cursor])) cursor++;
    static constexpr std::string_view ordinals[] = {"st", "nd", "rd", "th"};
    for (const std::string_view ordinal : ordinals) {
      if (starts_with_at(text, cursor, ordinal) && !is_word(code_point_at(text, cursor + ordinal.size())))
        return cursor + ordinal.size();
    }
    return std::nullopt;
  }
  if (!is_ascii_upper(text[cursor])) return std::nullopt;
  cursor++;
  for (;;) {
    if (cursor < text.size() && is_ascii_letter(text[cursor])) {
      cursor++;
      continue;
    }
    if (cursor + 1 < text.size() && (text[cursor] == '\'' || text[cursor] == '-') && is_ascii_letter(text[cursor + 1])) {
      cursor += 2;
      continue;
    }
    break;
  }
  if (is_word(code_point_at(text, cursor))) return std::nullopt;
  return cursor;
}

// The part of a building after an English street: ", Suite 400", " Apt. 5B",
// ", Unit C", ", 48th Floor", ", #1200". Its number starts with a digit or is
// a single capital, so "Suite of services" is no suite.
std::optional<std::size_t> english_unit_end(const std::string &text, std::size_t position) {
  std::size_t cursor = skip_spaces(text, position);
  if (cursor < text.size() && text[cursor] == ',') cursor = skip_spaces(text, cursor + 1);
  if (cursor == position || cursor >= text.size()) return std::nullopt;
  const auto numberEnd = [&](std::size_t valueStart) -> std::optional<std::size_t> {
    if (valueStart >= text.size()) return std::nullopt;
    if (is_ascii_digit(text[valueStart])) return word_run_end(text, valueStart, false);
    if (is_ascii_upper(text[valueStart]) && !is_word(code_point_at(text, valueStart + 1))) return valueStart + 1;
    return std::nullopt;
  };
  if (text[cursor] == '#') return numberEnd(cursor + 1);
  static constexpr std::string_view floorWord = "Floor";
  if (is_ascii_digit(text[cursor])) {
    const auto ordinalEnd = english_street_word_end(text, cursor);
    if (!ordinalEnd) return std::nullopt;
    const std::size_t floor = skip_spaces(text, *ordinalEnd);
    if (floor == *ordinalEnd || !starts_with_at(text, floor, floorWord)
        || is_word(code_point_at(text, floor + floorWord.size())))
      return std::nullopt;
    return floor + floorWord.size();
  }
  static constexpr std::string_view designators[] = {
      "Suite", "Ste.", "Ste", "Apartment", "Apt.", "Apt", "Unit", "Floor", "Fl.", "Room", "Rm.", "Building",
      "Bldg.", "Bldg",
  };
  for (const std::string_view designator : designators) {
    if (!starts_with_at(text, cursor, designator)) continue;
    const std::size_t designatorEnd = cursor + designator.size();
    if (designator.back() != '.' && is_word(code_point_at(text, designatorEnd))) continue;
    std::size_t valueStart = skip_spaces(text, designatorEnd);
    if (valueStart < text.size() && text[valueStart] == '#') valueStart++;
    if (valueStart == designatorEnd) continue;
    if (const auto end = numberEnd(valueStart)) return end;
  }
  return std::nullopt;
}

// The city, state and ZIP code after an English street: ", New York, NY
// 10118", ", Springfield, IL 62704", ", Austin, Texas 78701". A state spelled
// out counts only with its ZIP code, so "..., Acme Holdings, Delaware" is no
// city and state.
std::optional<std::size_t> city_state_zip_end(const std::string &text, std::size_t position) {
  if (position >= text.size() || text[position] != ',') return std::nullopt;
  const auto capitalisedWordsEnd = [&](std::size_t wordStart, int most) -> std::optional<std::size_t> {
    std::optional<std::size_t> end;
    for (int words = 0; words < most; ++words) {
      const auto wordEnd = english_street_word_end(text, wordStart);
      if (!wordEnd || !is_ascii_upper(text[wordStart]) || *wordEnd - wordStart < 2) break;
      end = *wordEnd;
      const std::size_t next = skip_spaces(text, *wordEnd);
      if (next == *wordEnd) break;
      wordStart = next;
    }
    return end;
  };
  const auto zipEnd = [&](std::size_t after) -> std::optional<std::size_t> {
    const std::size_t zipStart = skip_spaces(text, after);
    if (zipStart == after) return std::nullopt;
    std::size_t cursor = zipStart;
    while (cursor < text.size() && is_ascii_digit(text[cursor])) cursor++;
    if (cursor - zipStart != 5) return std::nullopt;
    if (cursor + 5 <= text.size() && text[cursor] == '-'
        && std::all_of(text.begin() + static_cast<std::ptrdiff_t>(cursor + 1),
                       text.begin() + static_cast<std::ptrdiff_t>(cursor + 5), is_ascii_digit))
      cursor += 5;
    if (is_word(code_point_at(text, cursor))) return std::nullopt;
    return cursor;
  };
  const auto cityEnd = capitalisedWordsEnd(skip_spaces(text, position + 1), 3);
  if (!cityEnd || *cityEnd >= text.size() || text[*cityEnd] != ',') return std::nullopt;
  const std::size_t state = skip_spaces(text, *cityEnd + 1);
  if (state + 2 <= text.size() && is_ascii_upper(text[state]) && is_ascii_upper(text[state + 1])
      && !is_word(code_point_at(text, state + 2))) {
    if (const auto zip = zipEnd(state + 2)) return zip;
    return state + 2;
  }
  const auto stateEnd = capitalisedWordsEnd(state, 2);
  if (!stateEnd) return std::nullopt;
  return zipEnd(*stateEnd);
}

// A street address as it is written in English: the house number, the
// street's name and its kind, then the suite or floor and the city, state and
// ZIP code when they follow - "1200 Main Street, Suite 400", "350 Fifth
// Avenue, New York, NY 10118". The semantic model finds the city at best and
// sends the street in clear. A number starts it and a capitalised name must
// follow, so "12 months", "Article 9", "Section 12.4" or "$30 million" is
// never one, and "Main Street" without a number is left to the model. A
// street without its kind ("10 Hudson Yards") counts only with a suite, a
// floor or a city after it - and so does "Court", which also ends "2 Supreme
// Court decisions".
std::optional<std::pair<std::size_t, std::size_t>> english_address_at(const std::string &text,
                                                                      std::size_t start) {
  static constexpr std::string_view kinds[] = {
      "Street", "Avenue", "Road", "Boulevard", "Lane", "Drive", "Parkway", "Highway", "Place", "Terrace", "Way",
      "Square", "Plaza", "Circle",
  };
  // Written with or without their full stop; the stop is part of the name.
  static constexpr std::string_view abbreviatedKinds[] = {
      "St", "Ave", "Rd", "Blvd", "Ln", "Dr", "Pkwy", "Hwy", "Pl", "Ter", "Sq", "Cir",
  };
  static constexpr std::string_view weakKinds[] = {"Court", "Ct"};
  static constexpr std::string_view directions[] = {"NW", "NE", "SW", "SE", "N", "S", "E", "W"};
  if (!is_ascii_digit(text[start])) return std::nullopt;
  // A figure glued to a sign or to another figure belongs to that: "$30",
  // "12.4", "1,200", "10:30", "§ 5".
  if (start > 0) {
    const char before = text[start - 1];
    if (before == '.' || before == ',' || before == '$' || before == '#' || before == '/' || before == '-'
        || before == ':')
      return std::nullopt;
  }
  if (start >= 2 && text.compare(start - 2, 2, "\xC2\xA7") == 0) return std::nullopt;
  if (start >= 3 && text.compare(start - 3, 3, "\xC2\xA7 ") == 0) return std::nullopt;
  std::size_t cursor = start;
  while (cursor < text.size() && is_ascii_digit(text[cursor])) cursor++;
  if (cursor - start > 6) return std::nullopt;
  if (cursor < text.size() && is_ascii_letter(text[cursor]) && !is_word(code_point_at(text, cursor + 1))) cursor++;
  const std::size_t numberEnd = cursor;
  cursor = skip_spaces(text, numberEnd);
  if (cursor == numberEnd) return std::nullopt;
  const auto listed = [](const std::string_view word, const auto &list) {
    return std::find(std::begin(list), std::end(list), word) != std::end(list);
  };
  std::optional<std::size_t> streetEnd;
  bool kind = false;
  bool weak = false;
  for (int words = 0; words < 6; ++words) {
    const auto wordEnd = english_street_word_end(text, cursor);
    if (!wordEnd) break;
    const std::string_view word(text.data() + cursor, *wordEnd - cursor);
    // The first word is always the street's own name: "5 Park Place", "12
    // St. Mary's Road".
    const bool named = streetEnd.has_value();
    const bool abbreviated = named && listed(word, abbreviatedKinds);
    kind = named && (listed(word, kinds) || abbreviated);
    weak = named && listed(word, weakKinds);
    std::size_t end = *wordEnd;
    // An abbreviation keeps its full stop ("St.", "Ct.", "N."); a sentence's
    // own full stop after "Street" stays outside.
    if (end < text.size() && text[end] == '.' && (abbreviated || word.size() <= 2)) end++;
    streetEnd = end;
    if (kind || weak) break;
    const std::size_t next = skip_spaces(text, end);
    if (next == end) break;
    cursor = next;
  }
  if (!streetEnd) return std::nullopt;
  std::size_t end = *streetEnd;
  if (kind) {
    const std::size_t next = skip_spaces(text, end);
    for (const std::string_view direction : directions) {
      if (next == end || !starts_with_at(text, next, direction)) continue;
      if (is_word(code_point_at(text, next + direction.size()))) continue;
      end = next + direction.size();
      break;
    }
  }
  bool unit = false;
  for (int units = 0; units < 2; ++units) {
    const auto unitEnd = english_unit_end(text, end);
    if (!unitEnd) break;
    end = *unitEnd;
    unit = true;
  }
  bool place = false;
  if (const auto placeEnd = city_state_zip_end(text, end)) {
    end = *placeEnd;
    place = true;
  }
  if (!kind && !unit && !place) return std::nullopt;
  return std::make_pair(start, end);
}

// A mailbox is a party's address as surely as a street is: "P.O. Box 61842",
// "PO Box 2210", "Post Office Box 77", with the city, state and ZIP code after
// it when they follow. The model saw "P.O. Box 61842" only weakly, and it left
// readable (round 6).
std::optional<std::pair<std::size_t, std::size_t>> english_mailbox_at(const std::string &text,
                                                                      std::size_t start) {
  static constexpr std::string_view openings[] = {"P.O. Box", "P. O. Box", "P.O.Box", "PO Box", "Post Office Box",
      "P.O. BOX", "PO BOX", "POST OFFICE BOX"};
  for (const std::string_view opening : openings) {
    if (!starts_with_at(text, start, opening)) continue;
    const std::size_t afterOpening = start + opening.size();
    const std::size_t digits = skip_spaces(text, afterOpening);
    if (digits == afterOpening) return std::nullopt;
    std::size_t cursor = digits;
    while (cursor < text.size() && is_ascii_digit(text[cursor])) cursor++;
    if (cursor == digits || cursor - digits > 8 || is_word(code_point_at(text, cursor))) return std::nullopt;
    std::size_t end = cursor;
    if (const auto placeEnd = city_state_zip_end(text, end)) end = *placeEnd;
    return std::make_pair(start, end);
  }
  return std::nullopt;
}

// A name word: a capital followed by small letters, hyphenated parts allowed
// ("Римский-Корсаков"). An all-capitals word ("ООО") is not a name.
std::optional<std::size_t> name_word_end(const std::string &text, std::size_t position) {
  const CodePoint first = code_point_at(text, position);
  if (!is_upper(first)) return std::nullopt;
  std::size_t cursor = position + first.length;
  if (!is_lower(code_point_at(text, cursor))) return std::nullopt;
  for (;;) {
    const CodePoint point = code_point_at(text, cursor);
    if (is_lower(point)) {
      cursor += point.length;
      continue;
    }
    if (point.value == '-') {
      const CodePoint after = code_point_at(text, cursor + 1);
      if (is_upper(after) || is_lower(after)) {
        cursor += 1 + after.length;
        continue;
      }
    }
    break;
  }
  if (is_word(code_point_at(text, cursor))) return std::nullopt;
  return cursor;
}

// One or two initials: "А.И.", "А. И.", "А.".
std::optional<std::size_t> initials_end(const std::string &text, std::size_t position) {
  std::optional<std::size_t> end;
  std::size_t cursor = position;
  for (int initials = 0; initials < 2; ++initials) {
    const CodePoint letter = code_point_at(text, cursor);
    if (!is_upper(letter)) break;
    const std::size_t dot = cursor + letter.length;
    if (dot >= text.size() || text[dot] != '.') break;
    end = dot + 1;
    cursor = skip_spaces(text, dot + 1);
  }
  return end;
}

// The person named after «ИП» (an individual entrepreneur): "ИП Медведев
// Алексей Игоревич", "ИП Медведев А.И.", "ИП А.И. Медведев". The semantic
// model reliably takes the first name and patronymic and leaves the surname
// right after "ИП" in clear. The span is the name alone, so "ИП" stays
// readable and says what kind of party this is.
std::optional<std::pair<std::size_t, std::size_t>> entrepreneur_name_at(const std::string &text,
                                                                        std::size_t start) {
  static constexpr std::string_view marker = "ИП";
  if (!starts_with_at(text, start, marker)) return std::nullopt;
  const std::size_t afterMarker = start + marker.size();
  const std::size_t nameStart = skip_spaces(text, afterMarker);
  if (nameStart == afterMarker) return std::nullopt;
  if (const auto leadingInitials = initials_end(text, nameStart)) {
    const auto surnameEnd = name_word_end(text, skip_spaces(text, *leadingInitials));
    if (!surnameEnd) return std::nullopt;
    return std::make_pair(nameStart, *surnameEnd);
  }
  const auto surnameEnd = name_word_end(text, nameStart);
  if (!surnameEnd) return std::nullopt;
  std::size_t end = *surnameEnd;
  std::size_t next = skip_spaces(text, end);
  if (next == end) return std::make_pair(nameStart, end);
  if (const auto trailingInitials = initials_end(text, next)) return std::make_pair(nameStart, *trailingInitials);
  // The first name and patronymic, when written out.
  for (int words = 0; words < 2; ++words) {
    const auto wordEnd = name_word_end(text, next);
    if (!wordEnd) break;
    end = *wordEnd;
    const std::size_t after = skip_spaces(text, end);
    if (after == end) break;
    next = after;
  }
  return std::make_pair(nameStart, end);
}

std::vector<Match> find_shape_matches(const std::string &text) {
  auto matches = find_at_word_starts(text, address_label, [&](std::size_t start) {
    if (const auto russian = russian_address_at(text, start)) return russian;
    if (const auto named = russian_named_street_at(text, start)) return named;
    if (const auto mailbox = english_mailbox_at(text, start)) return mailbox;
    return english_address_at(text, start);
  });
  auto names = find_at_word_starts(text, person_label,
                                   [&](std::size_t start) { return entrepreneur_name_at(text, start); });
  matches.insert(matches.end(), names.begin(), names.end());
  return matches;
}

// A reference-style identifier in any script: capitals, perhaps with a few
// figures after them, then two or three groups of figures joined by hyphens or
// slashes - "З-082/26", "СП-121/26", "ТЛ-14/2026", "А40-12345/2026". The same
// shape as identifier_pattern's first form, which reads Latin letters only: a
// Russian claims register (24 Sep 2026) sent four application numbers such as
// "З-082/26" in clear where the semantic model found the other four.
// Where a code's letter part ends: "/П", "/B", "-А" - a slash or a hyphen, one
// to three capitals, and nothing of a word after them. `position` itself when
// none stands there.
std::size_t code_letter_suffix_end(const std::string &text, std::size_t position) {
  if (position >= text.size() || (text[position] != '/' && text[position] != '-')) return position;
  std::size_t cursor = position + 1;
  std::size_t letters = 0;
  for (CodePoint point = code_point_at(text, cursor); is_upper(point) && letters <= 3; point = code_point_at(text, cursor)) {
    cursor += point.length;
    ++letters;
  }
  if (letters == 0 || letters > 3 || is_word(code_point_at(text, cursor))) return position;
  return cursor;
}

std::vector<Match> find_code_identifier_matches(const std::string &text) {
  const auto digitsAt = [&](std::size_t position) {
    std::size_t end = position;
    while (end < text.size() && is_ascii_digit(text[end])) ++end;
    return end - position;
  };
  return find_at_word_starts(text, to_string(Category::Identifiers),
                             [&](std::size_t start) -> std::optional<std::pair<std::size_t, std::size_t>> {
    std::size_t cursor = start;
    std::size_t letters = 0;
    for (CodePoint point = code_point_at(text, cursor); is_upper(point) && letters <= 4;
         point = code_point_at(text, cursor)) {
      cursor += point.length;
      ++letters;
    }
    if (letters == 0 || letters > 4) return std::nullopt;
    // A second part of capitals: "KR-OPT-2291".
    if (cursor + 1 < text.size() && text[cursor] == '-') {
      std::size_t second = cursor + 1;
      std::size_t more = 0;
      for (CodePoint point = code_point_at(text, second); is_upper(point) && more <= 5; point = code_point_at(text, second)) {
        second += point.length;
        ++more;
      }
      if (more >= 1 && more <= 5 && second < text.size() && text[second] == '-' && digitsAt(second + 1) >= 2)
        cursor = second;
    }
    const std::size_t prefixDigits = digitsAt(cursor);
    if (prefixDigits > 3) return std::nullopt;
    cursor += prefixDigits;
    int groups = 0;
    std::size_t widest = 0;
    while (groups < 3 && cursor < text.size()
           && (text[cursor] == '-' || text[cursor] == '/' || (text[cursor] == '.' && groups > 0))) {
      const std::size_t digits = digitsAt(cursor + 1);
      if (digits < 2 || digits > 6) break;
      cursor += 1 + digits;
      widest = std::max(widest, digits);
      ++groups;
    }
    // A code closed by a letter part - "ФП-044/П", "ИТ-112/25-А": one group
    // of figures and its letters are the code as much as two groups are.
    if (groups >= 1) {
      const std::size_t suffixEnd = code_letter_suffix_end(text, cursor);
      if (suffixEnd > cursor) {
        cursor = suffixEnd;
        ++groups;
      }
    }
    // One group is a code where a number's sign stands before it ("№ ФП-044"),
    // or where it has four figures or more after capitals: "Invoice AMS-44871",
    // "KR-OPT-2291". A short one - "COVID-19", "Covid-19", "B-52" - is a name.
    const bool oneGroup = groups == 1 && (after_number_sign(text, start) || (letters >= 2 && widest >= 4));
    if (groups < (oneGroup ? 1 : 2) || is_word(code_point_at(text, cursor)))
      return std::nullopt;
    return std::make_pair(start, cursor);
  });
}

// ---- What a number means where it stands ------------------------------------
//
// A number is protected or not by what is written around it, never by its
// length: the product owner rejected "fewer than four digits identify nobody"
// as a privacy rule (24 Sep 2026). A purchase price written "12" under
// "Purchase price ($m)" is the most sensitive number of a deal; "Row 10" and
// "12 months" are nobody's secret. The words that say what a number is are
// data, in English and Russian: an entry ending in '*' is a stem that every
// form beginning with it matches ("стоимост*": стоимость, стоимости,
// стоимостью), any other entry the whole word. Words are compared in lower
// case, "ё" as "е".

// Money named before a number or in a label: "purchase price of 12", "Fees",
// "Exposure", "Стоимость услуг, ₽", "Сумма убытков", "Итого".
constexpr std::string_view kMoneyWords[] = {
    "price", "prices", "priced", "pricing", "cost", "costs", "fee", "fees", "amount", "amounts", "payment",
    "payments", "pay", "pays", "paid", "payable", "repayment", "prepayment", "consideration", "salary", "salaries",
    "wage", "wages", "compensation", "remuneration", "bonus", "bonuses", "damages", "penalty", "penalties", "fine",
    "fines", "deposit", "deposits", "escrow", "holdback", "earnout", "earn-out", "revenue", "revenues", "sales",
    "turnover", "income", "profit", "profits", "loss", "losses", "ebitda", "ebit", "budget", "budgets", "valuation",
    "exposure", "exposures", "debt", "debts", "loan", "loans", "principal", "rent", "rental", "royalty",
    "royalties", "commission", "commissions", "retainer", "total", "totals", "subtotal", "sum", "sums", "balance",
    "balances", "capital", "proceeds", "dividend", "dividends", "expense", "expenses", "expenditure", "charge",
    "charges", "reimbursement", "settlement", "award", "funding", "investment", "cash", "cap", "caps", "liability",
    "liabilities", "premium", "premiums", "tax", "taxes", "tariff", "tariffs", "worth", "vat", "gst", "cam",
    "insurance", "utilities", "arrears", "ндс", "коммунальн*",
    // What is owed, awarded, bought or billed: "owes 12", "a judgment of 12", "invoiced 12".
    // Never a record's own name ("Claim 12", "Invoice 12345"), which may number it, nor "owes",
    // which counts things as often ("owes 12 x 100 units").
    "judgment", "judgments", "judgement", "judgements", "verdict", "claimed",
    "purchased", "invoiced", "billed", "refund", "refunds", "reimbursed",
    "уплат*", "перечисл*", "возмещ*", "взнос*", "взыскива*", "присужд*",
    "цена", "цены", "цене", "цену", "ценой", "ценою", "цен", "ценам", "ценами", "ценах", "стоимост*", "сумм*",
    "оплат*", "плата", "платы", "плате", "плату", "платой", "платою", "платам", "платами", "платах", "платеж*",
    "платн*", "выплат*", "вознагражден*", "неустойк*", "штраф*", "пени", "пеня", "пеней", "убыт*", "ущерб*",
    "компенсац*", "задолженност*", "долг", "долга", "долгу", "долгом", "долге", "долги", "долгов", "выручк*",
    "прибыл*", "доход*", "расход*", "затрат*", "бюджет*", "аванс*", "депозит*", "залог*", "кредит*", "займ*",
    "заем", "аренд*", "роялти", "комисси*", "гонорар*", "зарплат*", "заработн*", "оклад*", "преми*", "лимит*",
    "предел*", "итог", "итого", "итогов*", "остат*", "требовани*", "взыскан*", "размер*", "капитал*",
    "дивиденд*", "налог*", "тариф*",
};

// A financial ratio a company keeps confidential, named in a label: "Gross
// margin", "Ownership %", "Adjusted EBITDA margin", "IRR", "Доля участия".
constexpr std::string_view kRatioWords[] = {
    "margin", "margins", "ownership", "owned", "stake", "stakes", "shareholding", "shareholdings", "equity", "irr",
    "roi", "roe", "roic", "roa", "moic", "wacc", "cagr", "yield", "yields", "interest", "coupon", "dilution",
    "discount", "markup", "mark-up",
    "марж*", "рентабельност*", "доля", "доли", "долю", "долей", "долям", "долями", "долях", "владени*", "ставк*",
    "доходност*", "дисконт*", "процентн*", "наценк*", "скидк*",
};

// A party's holding named in a label or after a count: "Shares", "Units",
// "Membership Interests", «Количество акций», «паев».
constexpr std::string_view kHoldingWords[] = {
    "share", "shares", "units", "membership", "stock", "stockholding", "interests", "options", "warrants", "rsus",
    "rsu",
    "акци*", "пай", "паи", "паев", "паёв", "пая", "паю", "паем", "паями", "паях", "облигаци*",
};

// An identifier named before its number or in a label: "Account suffix 12",
// "ИНН 7710482917", "паспорт 45 18" ("р/с" and "к/с": see
// account_abbreviation_before()).
constexpr std::string_view kIdentifierWords[] = {
    "account", "accounts", "acct", "iban", "swift", "bic", "routing", "aba", "suffix", "passport", "passports", "ssn",
    "ein", "tin", "itin", "id", "identifier", "reference", "ref", "docket", "serial", "registration",
    // A vehicle's, a patient's, a driver's: "VIN 1HGCM82633A004352", "Medicare MBI 1EG4-TE5-MK73",
    // "Driver's license D1234567", "NPI 1234567893", "MRN 00482211".
    "vin", "mbi", "hicn", "npi", "mrn", "dl", "license", "licence",
    "инн", "огрн", "огрнип", "кпп", "бик", "окпо", "октмо", "окато", "снилс", "паспорт*", "серия", "серии",
    "номер", "номера", "номеру", "номером", "номере", "счет", "счета", "счету", "счетом", "счете", "счетов",
    "лицев*", "расчетн*", "корреспондентск*", "идентификатор*", "регистрационн*",
};

// A record a code identifies: "employee ID", "staff number", "policy No.",
// "табельный номер".
constexpr std::string_view kRecordWords[] = {
    "employee", "employees", "staff", "personnel", "member", "badge", "customer", "client", "account", "policy",
    "claim", "case", "file", "matter", "order", "invoice", "contract", "patient", "student", "vehicle", "loan",
    "табельный", "табельного", "сотрудника", "работника", "клиента", "полиса", "счета", "договора", "заявки",
    "претензии", "дела", "пропуска", "удостоверения",
};

// What names a record's code after the record: "No.", "number", "code", "номер".
constexpr std::string_view kCodeSigns[] = {"no", "nr", "number", "num", "code", "номер", "код"};

// A place in a document or a sheet, written before its number: "Row 10",
// "Section 12", "п. 9.4", "ст. 401", "Приложение № 2".
constexpr std::string_view kStructureWords[] = {
    "row", "rows", "column", "columns", "col", "cols", "cell", "cells", "line", "lines", "page", "pages", "p", "pp",
    "section", "sections", "sec", "article", "articles", "art", "clause", "clauses", "paragraph", "paragraphs",
    "para", "paras", "subparagraph", "subparagraphs", "subsection", "subsections", "subclause", "subclauses",
    "schedule", "schedules", "exhibit", "exhibits", "annex", "annexes", "appendix", "appendices", "attachment",
    "attachments", "part", "parts", "chapter", "chapters", "item", "items", "step", "steps", "point", "points",
    "rule", "rules", "footnote", "footnotes", "note", "notes", "figure", "figures", "fig", "table", "tables", "tab",
    "sheet", "sheets", "slide", "slides", "question", "questions", "title", "titles", "volume", "vol", "level",
    "levels", "stage", "stages", "phase", "phases", "round", "rounds", "tier", "tiers", "version", "ver", "draft",
    "edition",
    "п", "пп", "подп", "пункт*", "подпункт*", "ст", "статья", "статьи", "статье", "статью", "статьей", "статьею",
    "статей", "статьям", "статьями", "статьях", "раздел*", "подраздел*", "глава", "главы", "главе", "главу",
    "главой", "глав", "главам", "главами", "главах", "ч", "часть", "части", "частью", "частей", "частям", "частями",
    "частях", "абз", "абзац*", "приложени*", "строк*", "столбец", "столбца", "столбцу", "столбцом", "столбце",
    "столбцы", "столбцов", "лист", "листа", "листе", "листу", "листом", "листы", "листов", "страниц*", "стр",
    "таблиц*", "рис", "рисун*", "примечани*", "сноск*", "шаг", "шага", "шаге", "этап*", "вопрос*", "том", "тома",
    "томе", "уровень", "уровн*", "раунд*", "верси*", "редакци*", "позици*", "ячейк*",
};

// A length of time after a number: "12 months", "60 days", "12 месяцев".
constexpr std::string_view kTimeUnits[] = {
    "second", "seconds", "sec", "secs", "minute", "minutes", "min", "mins", "hour", "hours", "hr", "hrs", "day",
    "days", "week", "weeks", "wk", "wks", "month", "months", "mo", "mos", "year", "years", "yr", "yrs", "quarter",
    "quarters", "decade", "decades",
    "секунд*", "минут*", "час", "часа", "часов", "часам", "часами", "часах", "часу", "сутки", "суток", "сут",
    "день", "дня", "дней", "дням", "днями", "днях", "дн", "недел*", "месяц*", "мес", "квартал*", "год", "года",
    "году", "годом", "годам", "годами", "годах", "лет", "полугод*",
};

// What may stand between a number and its unit of time: "10 business days",
// "5 рабочих дней".
constexpr std::string_view kTimeQualifiers[] = {
    "business", "calendar", "working", "banking", "consecutive", "full", "additional", "rolling", "trading",
    "рабоч*", "календарн*", "банковск*", "полн*", "последовательн*", "дополнительн*",
};

// A count of things after a number: "3 copies", "2 экземпляра", "12 pages".
constexpr std::string_view kCountUnits[] = {
    "copy", "copies", "pcs", "piece", "pieces", "item", "items", "page", "pages", "times", "counterpart",
    "counterparts", "original", "originals",
    "шт", "штук*", "экземпляр*", "копия", "копии", "копий", "копиях", "копиями", "страниц*", "лист", "листа",
    "листах", "листов", "раз", "раза",
};

// A plain scalar, named before a number or in a label: "a probability of 0.64",
// "Weighting factor", "коэффициент 1,5".
constexpr std::string_view kScalarWords[] = {
    "factor", "factors", "coefficient", "coefficients", "multiplier", "weight", "weighting", "probability",
    "likelihood", "score", "scores", "rating", "ratings", "index", "count", "quantity", "qty", "headcount",
    "коэффициент*", "множител*", "вероятност*", "балл*", "рейтинг*", "индекс*", "количеств*", "кол-во", "число",
    "числа", "численност*",
};

// What joins a scalar's name to its number: "probability of 0.64".
constexpr std::string_view kScalarLinks[] = {"of", "is", "at", "equals", "равен", "равна", "равно", "составляет"};

// A currency after a number: "12 dollars", "2 064 рубля".
constexpr std::string_view kCurrencyWords[] = {
    "dollar", "dollars", "euro", "euros", "pound", "pounds", "sterling", "ruble", "rubles", "rouble", "roubles",
    "yen", "yuan", "renminbi", "franc", "francs", "cent", "cents",
    "руб", "рубл*", "коп", "копе*", "доллар*", "долл", "евро", "юан*", "фунт*", "франк*", "иен*", "цент", "цента",
    "центов", "тенге", "гривн*",
};

// A currency's code, before a number or after it: "USD 12", "12 EUR".
constexpr std::string_view kCurrencyCodes[] = {
    "usd", "eur", "gbp", "rub", "rur", "chf", "jpy", "cny", "rmb", "cad", "aud", "nzd", "sek", "nok", "dkk", "pln",
    "czk", "huf", "inr", "krw", "sgd", "hkd", "aed", "sar", "kzt", "uah", "byn", "zar", "brl", "mxn", "ils",
};

// A scale after a number: "12 million", "3,45 млн", "250 тыс.".
constexpr std::string_view kScaleWords[] = {
    "thousand", "thousands", "million", "millions", "billion", "billions", "trillion", "trillions", "mn", "mln",
    "bn", "tn",
    "тыс", "тысяч*", "млн", "миллион*", "млрд", "миллиард*", "трлн", "триллион*",
};

// A scale written glued to its number that says money by itself: "250k". The
// longer ones ("3.5bn", "12mn") are scale words.
constexpr std::string_view kGluedScales[] = {"k"};

// Letters that may close an amount already known to be money: "$12m", "$3.5b",
// "USD 12mm". Alone "12m" may be twelve months and "9B" a clause.
constexpr std::string_view kScaleSuffixes[] = {"k", "m", "mm", "b", "bn", "mn", "mln", "tn"};

// A percentage after a number: "15 %", "15 percent", "15 процентов", "10 bps".
constexpr std::string_view kPercentWords[] = {"percent", "percentage", "percentages", "pct", "bps", "bp", "процент*", "проц"};

// Number words restating an amount in brackets: "50 000 000 (пятидесяти
// миллионов) рублей", "1,000 (one thousand) dollars".
constexpr std::string_view kNumberWords[] = {
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve",
    "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty", "thirty", "forty",
    "fifty", "sixty", "seventy", "eighty", "ninety", "hundred", "hundreds", "thousand", "thousands", "million",
    "millions", "billion", "billions", "trillion", "and", "point",
    "нол*", "нул*", "один", "одн*", "два", "две", "двух", "двум", "двумя", "три", "трех", "трем", "тремя", "четыр*",
    "пят*", "шест*", "сем*", "восем*", "восьм*", "девят*", "десят*", "одиннадц*", "двенадц*", "тринадц*",
    "четырнадц*", "пятнадц*", "шестнадц*", "семнадц*", "восемнадц*", "девятнадц*", "двадцат*", "тридцат*",
    "сорок*", "девяност*", "сто", "ста", "стам", "стами", "стах", "сот*", "двест*", "трист*", "четырест*",
    "тысяч*", "тыс", "миллион*", "млн", "миллиард*", "млрд", "триллион*", "полтор*", "полутор*", "целых", "целая",
    "десятых", "сотых", "тысячных",
};

// A month, before or after a number: "March 2026", "15 марта".
constexpr std::string_view kMonthWords[] = {
    "january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november",
    "december", "jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept", "oct", "nov", "dec",
    "январ*", "феврал*", "март", "марта", "марте", "марту", "апрел*", "май", "мая", "мае", "маю", "июн*", "июл*",
    "август*", "сентябр*", "октябр*", "ноябр*", "декабр*",
};

// A year's mark after a number of a thousand or more: "2026 г.", "в 2026 году".
constexpr std::string_view kYearMarks[] = {"г", "гг", "год", "года", "году", "годом", "годе"};

// A period before a year: "FY 2026", "fiscal 2026".
constexpr std::string_view kPeriodWords[] = {"fy", "cy", "fiscal"};

// An ordinal's ending, glued to its number or after a hyphen: "5th", "2-й".
constexpr std::string_view kOrdinalEndings[] = {
    "st", "nd", "rd", "th", "й", "я", "е", "го", "ый", "ой", "ая", "ое", "ого", "ому", "ым", "ом", "ми", "х",
};

// A statute named by its title: "Закона о персональных данных", "the Law on
// Personal Data".
constexpr std::string_view kStatuteWords[] = {"закон", "закона", "закону", "законом", "законе", "law", "act", "code"};

// A statute's number is public law, not a document's identifier: "№ 152-ФЗ",
// "№ 87-ФЗ", "Federal Law No. 115-FZ".
constexpr std::string_view kStatuteSuffixes[] = {"фз", "фкз", "fz", "fkz"};

// "No.", "Nr." - a number's sign before it, like "№" and "#".
constexpr std::string_view kNumberSigns[] = {"no", "nr", "nos"};

// What joins two numbers of one kind: "Sections 5 and 12", "rows 1-12 of 17",
// "пп. 9.4 и 9.5".
constexpr std::string_view kNumberConnectors[] = {
    "to", "and", "or", "through", "thru", "of", "и", "или", "по", "до", "из", "от",
};

// What a reference to a place in a document may carry besides it: "Section 12
// of the Agreement", "разделом 10 Договора", "ст. 401 ГК РФ".
constexpr std::string_view kReferenceWords[] = {
    "of", "the", "this", "to", "in", "under", "new", "such", "said", "same", "existing", "amended", "following",
    "foregoing", "preceding", "agreement", "contract", "lease", "deed", "amendment", "договор*", "соглашени*",
    "контракт*", "настоящ*", "к", "в", "по", "гк", "гпк", "апк", "нк", "тк", "ук", "упк", "коап", "жк", "ск", "зк",
    "бк", "рф", "кодекс*",
};

template <std::size_t N>
bool is_cue(std::string_view word, const std::string_view (&cues)[N]) {
  for (const std::string_view cue : cues) {
    if (cue.back() == '*') {
      const std::string_view stem = cue.substr(0, cue.size() - 1);
      if (word.size() >= stem.size() && word.substr(0, stem.size()) == stem) return true;
    } else if (word == cue) {
      return true;
    }
  }
  return false;
}

bool is_letter_or_mark(const CodePoint &point) {
  switch (point.category) {
    case UTF8PROC_CATEGORY_LU:
    case UTF8PROC_CATEGORY_LL:
    case UTF8PROC_CATEGORY_LT:
    case UTF8PROC_CATEGORY_LM:
    case UTF8PROC_CATEGORY_LO:
    case UTF8PROC_CATEGORY_MN:
    case UTF8PROC_CATEGORY_MC:
    case UTF8PROC_CATEGORY_ME:
      return point.length > 0;
    default:
      return false;
  }
}

// The code point that ends right before `position`, and where it starts; a
// length of 0 when those bytes are not UTF-8.
std::pair<std::size_t, CodePoint> code_point_before(const std::string &text, std::size_t position) {
  if (position == 0) return {0, CodePoint{}};
  std::size_t start = position - 1;
  while (start > 0 && position - start < 4 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) --start;
  const CodePoint point = code_point_at(text, start);
  if (point.length == 0 || start + point.length != position) return {position - 1, CodePoint{}};
  return {start, point};
}

// A word in lower case, "ё" as "е".
std::string lowered_word(std::string_view word) {
  std::string result;
  result.reserve(word.size());
  std::size_t position = 0;
  while (position < word.size()) {
    utf8proc_int32_t value = -1;
    const utf8proc_ssize_t read = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(word.data() + position),
                                                   static_cast<utf8proc_ssize_t>(word.size() - position), &value);
    if (read <= 0) {
      result.push_back(word[position]);
      ++position;
      continue;
    }
    utf8proc_int32_t lower = utf8proc_tolower(value);
    if (lower == 0x0451) lower = 0x0435;
    utf8proc_uint8_t bytes[4];
    const utf8proc_ssize_t written = utf8proc_encode_char(lower, bytes);
    result.append(reinterpret_cast<const char *>(bytes), static_cast<std::size_t>(written));
    position += static_cast<std::size_t>(read);
  }
  return result;
}

// What joins two runs of digits into one number: "15.03.2026", "2,615,000",
// "10:30", "14/2026", "10-12".
bool joins_numbers(char character) {
  return character == '.' || character == ',' || character == '/' || character == ':' || character == '-';
}

std::size_t digits_from(const std::string &text, std::size_t position) {
  std::size_t end = position;
  while (end < text.size() && is_ascii_digit(text[end])) ++end;
  return end - position;
}

std::size_t digits_before(const std::string &text, std::size_t position) {
  std::size_t start = position;
  while (start > 0 && is_ascii_digit(text[start - 1])) --start;
  return position - start;
}

// The space that sets a group of three digits apart in "2 615 000": a plain,
// no-break, narrow no-break or thin space. Its length in bytes, or 0.
std::size_t group_space_at(const std::string &text, std::size_t position) {
  if (position >= text.size()) return 0;
  if (text[position] == ' ') return 1;
  if (text.compare(position, 2, "\xC2\xA0") == 0) return 2;
  if (text.compare(position, 3, "\xE2\x80\xAF") == 0 || text.compare(position, 3, "\xE2\x80\x89") == 0) return 3;
  return 0;
}

std::size_t group_space_before(const std::string &text, std::size_t position) {
  if (position >= 1 && text[position - 1] == ' ') return 1;
  if (position >= 2 && text.compare(position - 2, 2, "\xC2\xA0") == 0) return 2;
  if (position >= 3 && (text.compare(position - 3, 3, "\xE2\x80\xAF") == 0
                        || text.compare(position - 3, 3, "\xE2\x80\x89") == 0))
    return 3;
  return 0;
}

// Whether the three digits at `position` are a group that a space joins to
// the digits before it ("615" in "2 615 000"), and the same seen from the
// digits before the space ("2" in it).
bool grouped_with_digits_before(const std::string &text, std::size_t position) {
  if (digits_from(text, position) != 3) return false;
  const std::size_t space = group_space_before(text, position);
  if (space == 0) return false;
  const std::size_t left = digits_before(text, position - space);
  return left >= 1 && left <= 3;
}

bool grouped_with_digits_after(const std::string &text, std::size_t position) {
  const std::size_t left = digits_before(text, position);
  if (left < 1 || left > 3) return false;
  const std::size_t space = group_space_at(text, position);
  return space > 0 && digits_from(text, position + space) == 3;
}

// The whole number the digits at text[start, end) belong to: digits, what
// joins them (joins_numbers()), and groups of three a space sets apart.
std::pair<std::size_t, std::size_t> whole_number_bounds(const std::string &text, std::size_t start, std::size_t end) {
  for (;;) {
    if (start > 0 && is_ascii_digit(text[start - 1])) {
      --start;
      continue;
    }
    if (start >= 2 && start < text.size() && is_ascii_digit(text[start]) && joins_numbers(text[start - 1])
        && is_ascii_digit(text[start - 2])) {
      start -= 2;
      continue;
    }
    if (start < text.size() && is_ascii_digit(text[start]) && grouped_with_digits_before(text, start)) {
      start -= group_space_before(text, start) + 1;
      continue;
    }
    break;
  }
  for (;;) {
    if (end < text.size() && is_ascii_digit(text[end])) {
      ++end;
      continue;
    }
    if (end > 0 && end + 1 < text.size() && is_ascii_digit(text[end - 1]) && joins_numbers(text[end])
        && is_ascii_digit(text[end + 1])) {
      end += 2;
      continue;
    }
    if (end > 0 && is_ascii_digit(text[end - 1]) && grouped_with_digits_after(text, end)) {
      end += group_space_at(text, end) + 1;
      continue;
    }
    break;
  }
  return {start, end};
}

// Where the line holding `position` begins and ends, looked for no further
// than kLineReach bytes away.
constexpr std::size_t kLineReach = 8192;

std::size_t line_start_of(const std::string &text, std::size_t position) {
  const std::size_t floor = position > kLineReach ? position - kLineReach : 0;
  for (std::size_t cursor = position; cursor > floor; --cursor)
    if (text[cursor - 1] == '\n') return cursor;
  std::size_t start = floor;
  while (start < position && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) ++start;
  return start;
}

std::size_t line_end_of(const std::string &text, std::size_t position) {
  const std::size_t ceiling = std::min(text.size(), position + kLineReach);
  if (position >= ceiling) return ceiling;
  const std::size_t newline = std::string_view(text).substr(position, ceiling - position).find('\n');
  return newline == std::string_view::npos ? ceiling : position + newline;
}

// Inside a formula - in the braces the workbook reading writes one in,
// "{=D4*E4*F4}", or written out, "=B10+B11", "=SUM(G4:G11)*12": its numbers
// are cell references and constants, never values.
bool inside_formula(const std::string &text, std::size_t lineStart, std::size_t position) {
  const std::string_view line(text.data() + lineStart, position - lineStart);
  const std::size_t opening = line.rfind("{=");
  if (opening != std::string_view::npos && line.find('}', opening) == std::string_view::npos) return true;
  // Written out: "=" right before a cell reference or a function, and no space since.
  const std::size_t equals = line.rfind('=');
  if (equals == std::string_view::npos || equals + 1 >= line.size()) return false;
  const std::size_t space = line.find_first_of(" \t", equals);
  return space == std::string_view::npos
      && (is_ascii_upper(line[equals + 1]) || line[equals + 1] == '$' || line[equals + 1] == '(');
}

// "Row 12: " or "Row 6 (hidden): " at the start of a line of the workbook
// reading (XlsxExtractor.cpp): where the row's cells begin, or npos.
std::size_t row_cells_start(const std::string &text, std::size_t lineStart, std::size_t lineEnd) {
  std::size_t cursor = lineStart;
  while (cursor < lineEnd && (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
  if (text.compare(cursor, 4, "Row ") != 0) return std::string::npos;
  cursor += 4;
  const std::size_t digits = digits_from(text, cursor);
  if (digits == 0) return std::string::npos;
  cursor += digits;
  if (text.compare(cursor, 9, " (hidden)") == 0) cursor += 9;
  if (cursor + 2 > lineEnd || text.compare(cursor, 2, ": ") != 0) return std::string::npos;
  return cursor + 2;
}

// Where the line that opens a passage of the workbook reading has its span of
// rows - 'Sheet "Реестр", rows 14-17 of 17; header row 3: ...', 'Sheet
// "Assumptions" (hidden sheet), rows 1-4' - or npos when the line opens none.
std::size_t passage_rows_at(std::string_view line) {
  const std::size_t sheet = line.find("Sheet \"");
  if (sheet == std::string_view::npos) return std::string_view::npos;
  const std::size_t close = line.find('"', sheet + 7);
  if (close == std::string_view::npos) return std::string_view::npos;
  std::string_view rest = line.substr(close + 1);
  for (const std::string_view state : {std::string_view(" (hidden sheet)"), std::string_view(" (very hidden sheet)")})
    if (rest.substr(0, state.size()) == state) rest.remove_prefix(state.size());
  if (rest.substr(0, 7) != ", rows ") return std::string_view::npos;
  return static_cast<std::size_t>(rest.data() - line.data());
}

bool opens_passage(std::string_view line) { return passage_rows_at(line) != std::string_view::npos; }

// Whether the number starting at `position` is written glued to a word before
// it, as the number of a code or a reference: after letters ("B10", "E80",
// "H1", "ИНН7710482917"), after letters and a hyphen, slash, underscore or
// dollar sign ("СП-087", "ТЛ-14", "$B$3"), or after a phone's plus ("+7").
// "US$3" is money, not a code.
bool glued_before(const std::string &text, std::size_t position) {
  if (position == 0) return false;
  const char before = text[position - 1];
  if (before == '+') return true;
  std::size_t end = position;
  if (before == '-' || before == '/' || before == '_' || before == '$') {
    end = position - 1;
    if (before == '$') {
      std::size_t letters = end;
      while (letters > 0 && is_ascii_letter(text[letters - 1]) && end - letters < 3) --letters;
      if (letters < end && (letters == 0 || !is_ascii_letter(text[letters - 1]))) {
        std::string prefix = text.substr(letters, end - letters);
        std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static constexpr std::string_view currencies[] = {"us", "nz", "hk", "nt", "mx", "sg", "au", "ca", "c", "a", "s", "r"};
        const bool currency = std::find(std::begin(currencies), std::end(currencies), prefix) != std::end(currencies);
        if (currency && !inside_formula(text, line_start_of(text, position), position)) return false;
      }
    }
  }
  if (end == 0) return false;
  const CodePoint point = code_point_before(text, end).second;
  return point.length > 0 && is_letter_or_mark(point);
}

// Whether the digits at `position` follow a cell reference's letters: one to
// three capitals, perhaps an anchoring "$", and nothing joined before them -
// "B10", "E80", "AA12", "$B$3", "H1". A hyphen makes a code: "MR-20417".
bool cell_reference_shape(const std::string &text, std::size_t position) {
  std::size_t cursor = position;
  if (cursor > 0 && text[cursor - 1] == '$') --cursor;
  std::size_t letters = 0;
  while (cursor > 0 && is_ascii_upper(text[cursor - 1])) {
    --cursor;
    if (++letters > 3) return false;
  }
  if (letters == 0) return false;
  if (cursor == 0) return true;
  const char before = text[cursor - 1];
  if (before == '$' || before == '!' || before == ':' || before == '(' || before == '{' || before == '=' || before == '+'
      || before == '*' || before == '/' || before == ',' || before == ' ' || before == '\t' || before == '\n')
    return true;
  const CodePoint point = code_point_before(text, cursor).second;
  return point.length > 0 && !is_letter_or_mark(point) && !is_digit(point) && before != '-' && before != '_';
}

// Whether the letters glued before the digits at `position` make a workbook's
// cell reference - "B10", "$B$3", "AA12" - in the workbook reading's own lines
// or inside a formula, where such a thing is a place, never a value.
bool reads_as_cell_reference(const std::string &text, std::size_t position) {
  std::size_t cursor = position;
  if (cursor > 0 && text[cursor - 1] == '$') --cursor;
  std::size_t letters = 0;
  while (cursor > 0 && is_ascii_upper(text[cursor - 1])) {
    --cursor;
    if (++letters > 3) return false;
  }
  if (letters == 0 || digits_from(text, position) > 7) return false;
  if (cursor > 0) {
    const CodePoint point = code_point_before(text, cursor).second;
    if (point.length > 0 && (is_letter_or_mark(point) || is_digit(point))) return false;
  }
  const std::size_t lineStart = line_start_of(text, position);
  const std::size_t lineEnd = line_end_of(text, position);
  return row_cells_start(text, lineStart, lineEnd) != std::string::npos
      || opens_passage(std::string_view(text.data() + lineStart, lineEnd - lineStart))
      || inside_formula(text, lineStart, position);
}

// The code a glued number is written in: back over the letters, digits and
// inner hyphens, slashes, underscores and dollar signs, and on to their end -
// "СП-087/26", "ИНН7710482917", "р/с40702810400020004817".
std::pair<std::size_t, std::size_t> code_bounds(const std::string &text, std::size_t start, std::size_t end) {
  const auto codeCharacter = [](const CodePoint &point) {
    return point.length > 0 && (is_letter_or_mark(point) || is_digit(point));
  };
  const auto joiner = [](char character) {
    return character == '-' || character == '/' || character == '_' || character == '$';
  };
  for (;;) {
    const auto [previous, point] = code_point_before(text, start);
    if (codeCharacter(point)) {
      start = previous;
      continue;
    }
    if (start >= 2 && joiner(text[start - 1]) && codeCharacter(code_point_before(text, start - 1).second)) {
      start = code_point_before(text, start - 1).first;
      continue;
    }
    break;
  }
  for (;;) {
    const CodePoint point = code_point_at(text, end);
    if (codeCharacter(point)) {
      end += point.length;
      continue;
    }
    if (end + 1 < text.size() && joiner(text[end]) && codeCharacter(code_point_at(text, end + 1))) {
      end += 1 + code_point_at(text, end + 1).length;
      continue;
    }
    break;
  }
  return {start, end};
}

// A word (letters, with an inner hyphen or apostrophe: "earn-out", "кол-во"),
// a number (digits with what joins them) or any other single mark.
struct Lexeme {
  enum class Kind { Word, Number, Mark };
  Kind kind = Kind::Mark;
  std::size_t start = 0;
  std::size_t end = 0;
  // A word in lower case; a number or a mark as written.
  std::string text;

  bool mark(std::string_view wanted) const { return kind == Kind::Mark && text == wanted; }
  bool word(std::string_view wanted) const { return kind == Kind::Word && text == wanted; }
};

std::vector<Lexeme> lexemes(const std::string &text, std::size_t from, std::size_t to) {
  std::vector<Lexeme> items;
  to = std::min(to, text.size());
  while (from < to && (static_cast<unsigned char>(text[from]) & 0xC0) == 0x80) ++from;
  std::size_t position = from;
  while (position < to) {
    const CodePoint point = code_point_at(text, position);
    if (point.length == 0) {
      ++position;
      continue;
    }
    const std::size_t next = position + point.length;
    if (point.category == UTF8PROC_CATEGORY_ZS || point.category == UTF8PROC_CATEGORY_ZL
        || point.category == UTF8PROC_CATEGORY_ZP || point.category == UTF8PROC_CATEGORY_CC) {
      position = next;
      continue;
    }
    if (is_digit(point)) {
      std::size_t end = next;
      for (;;) {
        const CodePoint more = code_point_at(text, end);
        if (end < to && is_digit(more)) {
          end += more.length;
          continue;
        }
        if (end + 1 < to && joins_numbers(text[end]) && is_ascii_digit(text[end + 1])) {
          ++end;
          continue;
        }
        break;
      }
      items.push_back({Lexeme::Kind::Number, position, end, text.substr(position, end - position)});
      position = end;
      continue;
    }
    if (is_letter_or_mark(point)) {
      std::size_t end = next;
      for (;;) {
        const CodePoint more = code_point_at(text, end);
        if (end < to && is_letter_or_mark(more)) {
          end += more.length;
          continue;
        }
        if (end + 1 < to && (text[end] == '-' || text[end] == '\'') && is_letter_or_mark(code_point_at(text, end + 1))) {
          end += 1 + code_point_at(text, end + 1).length;
          continue;
        }
        break;
      }
      items.push_back({Lexeme::Kind::Word, position, end,
                       lowered_word(std::string_view(text).substr(position, end - position))});
      position = end;
      continue;
    }
    items.push_back({Lexeme::Kind::Mark, position, next, text.substr(position, point.length)});
    position = next;
  }
  return items;
}

bool is_currency_mark(const Lexeme &item) {
  return item.kind == Lexeme::Kind::Mark && code_point_at(item.text, 0).category == UTF8PROC_CATEGORY_SC;
}

// What a number's digits are shaped as: one plain number however it is
// grouped ("12", "0.64", "2,615,000", "2 615 000,50"), a date or a time
// ("15.03.2026", "2026-03-15", "03/2026", "10:30"), a clause's number
// ("10.3.4"), or some other run of numbers ("10-12", "27/26", "555-01-47").
enum class NumberShape { Plain, Date, Time, Clause, Compound };

NumberShape number_shape(std::string_view core) {
  std::vector<std::string_view> groups;
  std::string separators;
  std::size_t position = 0;
  while (position < core.size()) {
    const std::size_t start = position;
    while (position < core.size() && is_ascii_digit(core[position])) ++position;
    groups.push_back(core.substr(start, position - start));
    if (position >= core.size()) break;
    if (static_cast<unsigned char>(core[position]) >= 0x80 || core[position] == ' ') {
      separators.push_back(' ');
      while (position < core.size() && !is_ascii_digit(core[position])) ++position;
    } else {
      separators.push_back(core[position]);
      ++position;
    }
  }
  if (separators.empty()) return NumberShape::Plain;
  const auto value = [](std::string_view digits) {
    std::uint64_t number = 0;
    for (const char digit : digits.substr(0, 9)) number = number * 10 + static_cast<std::uint64_t>(digit - '0');
    return number;
  };
  const std::size_t count = separators.size();
  // Groups of three after the first, perhaps a decimal part after the last.
  const char grouping = separators.front();
  if (grouping == ',' || grouping == '.' || grouping == ' ') {
    std::size_t same = 0;
    while (same < count && separators[same] == grouping) ++same;
    const bool decimalLast = same == count - 1 && (separators.back() == '.' || separators.back() == ',')
        && separators.back() != grouping;
    if (same == count || decimalLast) {
      bool thousands = groups.front().size() >= 1 && groups.front().size() <= 3;
      for (std::size_t index = 1; index <= same && thousands; ++index) thousands = groups[index].size() == 3;
      if (thousands || (count == 1 && grouping != ' ')) {
        // "12.2026", "03.2026": a month and its year.
        if (count == 1 && grouping == '.' && groups[0].size() <= 2 && groups[1].size() == 4 && value(groups[0]) >= 1
            && value(groups[0]) <= 12 && value(groups[1]) >= 1900 && value(groups[1]) <= 2100)
          return NumberShape::Date;
        return NumberShape::Plain;
      }
    }
  }
  if (count == 1 && separators[0] == '/' && groups[0].size() <= 2 && groups[1].size() == 4 && value(groups[0]) >= 1
      && value(groups[0]) <= 12 && value(groups[1]) >= 1900 && value(groups[1]) <= 2100)
    return NumberShape::Date;
  if (std::all_of(separators.begin(), separators.end(), [](char separator) { return separator == ':'; }) && count <= 2
      && std::all_of(groups.begin(), groups.end(), [](std::string_view group) { return group.size() <= 2; }))
    return NumberShape::Time;
  if (count == 2 && separators[0] == separators[1]
      && (separators[0] == '.' || separators[0] == '/' || separators[0] == '-')) {
    const std::uint64_t first = value(groups[0]);
    const std::uint64_t second = value(groups[1]);
    const std::uint64_t third = value(groups[2]);
    const bool dayMonthYear = groups[0].size() <= 2 && groups[1].size() <= 2
        && (groups[2].size() == 2 || groups[2].size() == 4) && first >= 1 && first <= 31 && second >= 1
        && second <= 31 && (first <= 12 || second <= 12);
    const bool yearMonthDay = groups[0].size() == 4 && groups[1].size() <= 2 && groups[2].size() <= 2 && second >= 1
        && second <= 12 && third >= 1 && third <= 31;
    if (dayMonthYear || yearMonthDay) return NumberShape::Date;
  }
  if (std::all_of(separators.begin(), separators.end(), [](char separator) { return separator == '.'; }))
    return NumberShape::Clause;
  return NumberShape::Compound;
}

// The value a number's first run of digits makes, for telling a year from a
// count ("2026 год", "2 года").
std::uint64_t leading_value(std::string_view core) {
  std::uint64_t number = 0;
  for (std::size_t index = 0; index < core.size() && index < 15 && is_ascii_digit(core[index]); ++index)
    number = number * 10 + static_cast<std::uint64_t>(core[index] - '0');
  return number;
}

// One cell of a row of the workbook reading, split at " | ": its column ("B")
// and where its value begins, after "B5 = " (or "B5: ", as the reading wrote
// cells before 24 Sep 2026).
struct SheetCell {
  std::size_t start = 0;
  std::size_t end = 0;
  std::size_t value_start = 0;
  std::string column;
};

std::vector<SheetCell> sheet_cells(const std::string &text, std::size_t cellsStart, std::size_t lineEnd) {
  std::vector<SheetCell> cells;
  std::size_t cursor = cellsStart;
  while (cursor < lineEnd) {
    const std::size_t found = std::string_view(text).substr(cursor, lineEnd - cursor).find(" | ");
    const std::size_t separator = found == std::string_view::npos ? lineEnd : cursor + found;
    SheetCell cell{cursor, separator, cursor, {}};
    std::size_t letters = cursor;
    while (letters < separator && is_ascii_upper(text[letters])) ++letters;
    const std::size_t rowDigits = digits_from(text, letters);
    if (letters > cursor && letters - cursor <= 3 && rowDigits > 0) {
      const std::size_t after = letters + rowDigits;
      if (text.compare(after, 3, " = ") == 0) cell.value_start = after + 3;
      else if (text.compare(after, 2, ": ") == 0) cell.value_start = after + 2;
      if (cell.value_start != cursor) cell.column = text.substr(cursor, letters - cursor);
    }
    cell.value_start = std::min(cell.value_start, separator);
    cells.push_back(std::move(cell));
    cursor = separator + 3;
  }
  return cells;
}

// Where a cell's own text ends: before the reading's notes " (shown ...)",
// " {=...}", " (merged ...)" and " (hidden column)".
std::size_t cell_text_end(const std::string &text, const SheetCell &cell) {
  std::size_t end = cell.end;
  const std::string_view value(text.data() + cell.value_start, cell.end - cell.value_start);
  for (const std::string_view note : {std::string_view(" (shown "), std::string_view(" {="),
                                      std::string_view(" (merged "), std::string_view(" (hidden column)")}) {
    const std::size_t found = value.find(note);
    if (found != std::string_view::npos) end = std::min(end, cell.value_start + found);
  }
  return end;
}

// Whether text[start, end) is words rather than a number, a date or a code: it
// does not begin with a figure, and it has a run of `letters` letters.
bool holds_words(const std::string &text, std::size_t start, std::size_t end, std::size_t letters) {
  std::size_t cursor = start;
  while (cursor < end && text[cursor] == ' ') ++cursor;
  if (cursor < end && (is_ascii_digit(text[cursor])
                       || ((text[cursor] == '-' || text[cursor] == '(') && cursor + 1 < end
                           && is_ascii_digit(text[cursor + 1]))))
    return false;
  std::size_t run = 0;
  while (cursor < end) {
    const CodePoint point = code_point_at(text, cursor);
    if (point.length == 0) {
      ++cursor;
      run = 0;
      continue;
    }
    run = is_letter_or_mark(point) ? run + 1 : 0;
    if (run >= letters) return true;
    cursor += point.length;
  }
  return false;
}

// One header cell of a workbook passage: its column and where its words are.
struct HeaderLabel {
  std::string column;
  std::size_t start = 0;
  std::size_t end = 0;
};

// How far back a workbook row looks for the line that opens its passage.
constexpr std::size_t kPassageReach = 8192;

// The header row the passage of a workbook row opens with: the "header row 3:
// ..." its opening line repeats or, in the passage that holds the header row
// itself, the first row before this one whose two or more cells all hold words.
std::vector<HeaderLabel> passage_header(const std::string &text, std::size_t rowLineStart) {
  static constexpr std::string_view opening = "Sheet \"";
  std::vector<HeaderLabel> labels;
  const std::size_t floor = rowLineStart > kPassageReach ? rowLineStart - kPassageReach : 0;
  const std::string_view window(text.data() + floor, rowLineStart - floor);
  // The nearest opening line is this row's passage's; a row never takes the
  // header of another sheet before it.
  for (std::size_t found = window.rfind(opening); found != std::string_view::npos;
       found = found == 0 ? std::string_view::npos : window.rfind(opening, found - 1)) {
    const std::size_t at = floor + found;
    const std::size_t lineEnd = std::min(line_end_of(text, at), rowLineStart);
    const std::string_view line(text.data() + at, lineEnd - at);
    if (!opens_passage(line)) {
      // "Sheet \"" inside a cell's words is no opening; one at a line's start is,
      // and it is another passage's.
      if (at == 0 || text[at - 1] == '\n') return labels;
      continue;
    }
    const auto take = [&](std::size_t cellsStart, std::size_t cellsEnd) {
      for (const SheetCell &cell : sheet_cells(text, cellsStart, cellsEnd))
        if (!cell.column.empty()) labels.push_back({cell.column, cell.value_start, cell_text_end(text, cell)});
    };
    const std::size_t header = line.find("; header row ");
    if (header != std::string_view::npos) {
      const std::size_t colon = line.find(": ", header);
      if (colon != std::string_view::npos) take(at + colon + 2, lineEnd);
      return labels;
    }
    for (std::size_t cursor = lineEnd + 1; cursor < rowLineStart;) {
      const std::size_t end = std::min(line_end_of(text, cursor), rowLineStart);
      const std::size_t cellsStart = row_cells_start(text, cursor, end);
      if (cellsStart != std::string::npos) {
        const std::vector<SheetCell> cells = sheet_cells(text, cellsStart, end);
        const bool headerRow = cells.size() >= 2 && std::all_of(cells.begin(), cells.end(), [&](const SheetCell &cell) {
          return !cell.column.empty() && holds_words(text, cell.value_start, cell_text_end(text, cell), 1);
        });
        if (headerRow) {
          take(cellsStart, end);
          return labels;
        }
      }
      cursor = end + 1;
    }
    return labels;
  }
  return labels;
}

// What has been read of the line a number stands on: whether it is a row of
// the workbook reading, its cells and, once asked for, its passage's header.
// Kept while numbers of one line are judged.
struct LineReading {
  std::size_t line_start = std::string::npos;
  std::size_t cells_start = std::string::npos;
  std::vector<SheetCell> cells;
  bool header_read = false;
  std::vector<HeaderLabel> header;

  void read(const std::string &text, std::size_t lineStart, std::size_t lineEnd) {
    if (line_start == lineStart) return;
    line_start = lineStart;
    cells_start = row_cells_start(text, lineStart, lineEnd);
    cells = cells_start == std::string::npos ? std::vector<SheetCell>{} : sheet_cells(text, cells_start, lineEnd);
    header_read = false;
    header.clear();
  }

  const std::vector<HeaderLabel> &passage_header_of(const std::string &text) {
    if (!header_read) {
      header = passage_header(text, line_start);
      header_read = true;
    }
    return header;
  }
};

// What a label's words name: a financial ratio, money, an identifier, or
// structure and plain scalars ("Days overdue", "Term (months)", "Weighting
// factor") - the first of these it names, in that order.
NumberMeaning label_meaning(const std::vector<Lexeme> &items, std::size_t from, std::size_t to) {
  bool ratio = false;
  bool money = false;
  bool identifier = false;
  bool generic = false;
  bool holding = false;
  // The unit a label gives its values in brackets says what they are, whatever
  // its other words: "Purchase price ($m)" is money, "Cap (months)" a length of
  // time, "Share of fees (%)" a percentage.
  bool percent = false;
  for (std::size_t open = from; open < to; ++open) {
    if (!items[open].mark("(")) continue;
    std::size_t close = open + 1;
    while (close < to && close - open <= 4 && !items[close].mark(")")) ++close;
    if (close >= to || !items[close].mark(")")) continue;
    bool unitMoney = false;
    bool unitTime = false;
    for (std::size_t inner = open + 1; inner < close; ++inner) {
      const Lexeme &item = items[inner];
      if (is_currency_mark(item)) unitMoney = true;
      else if (item.mark("%")) percent = true;
      else if (item.kind != Lexeme::Kind::Word) continue;
      else if (is_cue(item.text, kCurrencyWords) || is_cue(item.text, kCurrencyCodes) || is_cue(item.text, kScaleWords))
        unitMoney = true;
      else if (is_cue(item.text, kTimeUnits) || is_cue(item.text, kCountUnits)) unitTime = true;
    }
    if (unitMoney) return NumberMeaning::Money;
    if (unitTime) return NumberMeaning::Generic;
  }
  for (std::size_t index = from; index < to; ++index) {
    const Lexeme &item = items[index];
    if (item.kind == Lexeme::Kind::Word) {
      // "Policy No.", "Staff number", "Табельный номер": a record's code.
      if (is_cue(item.text, kCodeSigns) && index > from && items[index - 1].kind == Lexeme::Kind::Word
          && is_cue(items[index - 1].text, kRecordWords))
        identifier = true;
      if (is_cue(item.text, kHoldingWords)) holding = true;
      else if (is_cue(item.text, kRatioWords)) ratio = true;
      else if (is_cue(item.text, kMoneyWords) || is_cue(item.text, kCurrencyWords) || is_cue(item.text, kCurrencyCodes)
               || is_cue(item.text, kScaleWords))
        money = true;
      else if (is_cue(item.text, kIdentifierWords)) identifier = true;
      else if (is_cue(item.text, kTimeUnits) || is_cue(item.text, kStructureWords) || is_cue(item.text, kScalarWords)
               || is_cue(item.text, kCountUnits))
        generic = true;
    } else if (is_currency_mark(item)) {
      money = true;
    } else if (item.mark("\xE2\x84\x96") || item.mark("#")) {
      identifier = true;
    }
  }
  if (percent) return ratio || money || holding ? NumberMeaning::FinancialRatio : NumberMeaning::Generic;
  if (ratio) return NumberMeaning::FinancialRatio;
  if (holding && !money && !identifier) return NumberMeaning::Holding;
  if (money) return NumberMeaning::Money;
  if (identifier) return NumberMeaning::Identifier;
  if (generic) return NumberMeaning::Generic;
  return NumberMeaning::Unknown;
}

NumberMeaning label_meaning(const std::string &text, std::size_t start, std::size_t end) {
  const std::vector<Lexeme> items = lexemes(text, start, end);
  return label_meaning(items, 0, items.size());
}

// Whether items[index] ends a clause: ";", "!", "?", "|", or a full stop that
// is not an abbreviation's ("п.", "руб.", "No.", "Inc.").
bool ends_clause(const std::vector<Lexeme> &items, std::size_t index) {
  const Lexeme &item = items[index];
  if (item.kind != Lexeme::Kind::Mark) return false;
  if (item.text == ";" || item.text == "!" || item.text == "?" || item.text == "|") return true;
  if (item.text != ".") return false;
  if (index > 0 && items[index - 1].kind == Lexeme::Kind::Word && items[index - 1].end == item.start) {
    const std::string &word = items[index - 1].text;
    const auto letters = std::count_if(word.begin(), word.end(),
                                       [](char byte) { return (static_cast<unsigned char>(byte) & 0xC0) != 0x80; });
    if (letters <= 4) return false;
  }
  return true;
}

// Whether items[index] says "per cent" in any of its forms: "%" aside, "15
// percent", "15 per cent", "10 bps", "10 basis points", "15 процентов", "2 п.п.".
bool is_percent_at(const std::vector<Lexeme> &items, std::size_t index) {
  const Lexeme &item = items[index];
  if (item.kind != Lexeme::Kind::Word) return false;
  if (is_cue(item.text, kPercentWords)) return true;
  const auto nextIs = [&](std::size_t offset, std::string_view word) {
    return index + offset < items.size() && items[index + offset].word(word);
  };
  if (item.text == "per" && nextIs(1, "cent")) return true;
  if (item.text == "basis" && (nextIs(1, "points") || nextIs(1, "point"))) return true;
  return (item.text == "п" || item.text == "б") && index + 3 < items.size() && items[index + 1].mark(".")
      && nextIs(2, "п") && items[index + 3].mark(".");
}

// What the words right after a number say of it.
struct AfterCue {
  bool decided = false;
  NumberMeaning meaning = NumberMeaning::Unknown;
  // A percentage or a multiple: a financial ratio only under a financial label.
  bool ratio = false;
};

AfterCue decided(NumberMeaning meaning) { return AfterCue{true, meaning, false}; }
AfterCue as_ratio() { return AfterCue{true, NumberMeaning::Unknown, true}; }

// An opening bracket after a number: the workbook reading's note - "(shown in
// $)", "(shown in ₽)", "(shown as a percentage)" - or the amount restated in
// words, with or before its unit: "(пятидесяти миллионов) рублей", "(one
// thousand dollars)", "(twelve) months".
AfterCue bracket_cue(const std::vector<Lexeme> &after, std::size_t open) {
  std::size_t index = open + 1;
  if (index < after.size() && after[index].word("shown")) {
    bool money = false;
    bool ratio = false;
    for (++index; index < after.size() && !after[index].mark(")"); ++index) {
      if (after[index].word("percentage") || after[index].word("multiple")) ratio = true;
      // "in $", "in ₽", "in USD", "in thousands": a currency or a scale.
      if (after[index].word("in") && index + 1 < after.size() && !after[index + 1].mark(")")) money = true;
    }
    if (money) return decided(NumberMeaning::Money);
    if (ratio) return as_ratio();
    return {};
  }
  bool words = false;
  bool currency = false;
  for (; index < after.size() && after[index].kind == Lexeme::Kind::Word; ++index) {
    if (is_cue(after[index].text, kCurrencyWords) || is_cue(after[index].text, kCurrencyCodes)) currency = true;
    else if (!is_cue(after[index].text, kNumberWords)) return {};
    words = true;
  }
  if (!words || index >= after.size() || !after[index].mark(")")) return {};
  if (currency) return decided(NumberMeaning::Money);
  if (index + 1 >= after.size()) return {};
  const Lexeme &unit = after[index + 1];
  if (is_currency_mark(unit)) return decided(NumberMeaning::Money);
  if (unit.mark("%")) return as_ratio();
  if (unit.kind != Lexeme::Kind::Word) return {};
  if (is_cue(unit.text, kCurrencyWords) || is_cue(unit.text, kCurrencyCodes) || is_cue(unit.text, kScaleWords))
    return decided(NumberMeaning::Money);
  if (is_percent_at(after, index + 1)) return as_ratio();
  // "15 (пятнадцати) рабочих дней", "10 (ten) business days".
  const bool qualified = is_cue(unit.text, kTimeQualifiers) && index + 2 < after.size()
      && after[index + 2].kind == Lexeme::Kind::Word && is_cue(after[index + 2].text, kTimeUnits);
  if (qualified || is_cue(unit.text, kTimeUnits) || is_cue(unit.text, kCountUnits))
    return decided(NumberMeaning::Generic);
  return {};
}

// What the words right after a number say of it: a currency or a scale makes
// it money ("12 million", "12 рублей", "$" after it); a unit of time or a count
// makes it structure ("12 months", "3 copies"); a month or a year's mark makes
// it part of a date ("15 марта", "2026 г."); "%" makes it a percentage.
AfterCue after_cue(const std::vector<Lexeme> &after, std::size_t numberEnd, bool bracketed, std::uint64_t value) {
  std::size_t index = 0;
  // "sixty (60) days": past the bracket the number is written in.
  if (bracketed && index < after.size() && after[index].mark(")")) ++index;
  if (index >= after.size()) return {};
  const Lexeme &next = after[index];
  const bool glued = next.start == numberEnd;
  if (next.kind == Lexeme::Kind::Word) {
    const std::string &word = next.text;
    if (glued) {
      if (is_cue(word, kGluedScales) || is_cue(word, kScaleWords) || is_cue(word, kCurrencyWords)
          || is_cue(word, kCurrencyCodes))
        return decided(NumberMeaning::Money);
      if (word == "x") return as_ratio();
      if (value >= 1000 && is_cue(word, kYearMarks)) return decided(NumberMeaning::DatePart);
      if (is_cue(word, kOrdinalEndings)) return decided(NumberMeaning::Generic);
    }
    if (is_cue(word, kCurrencyWords) || is_cue(word, kCurrencyCodes) || is_cue(word, kScaleWords))
      return decided(NumberMeaning::Money);
    // "12 р.", roubles.
    if (word == "р" && index + 1 < after.size() && after[index + 1].mark(".") && after[index + 1].start == next.end)
      return decided(NumberMeaning::Money);
    if (is_percent_at(after, index)) return as_ratio();
    if (is_cue(word, kMonthWords)) return decided(NumberMeaning::DatePart);
    if (value >= 1000 && is_cue(word, kYearMarks)) return decided(NumberMeaning::DatePart);
    // "3,600 shares", «1 200 акций»: a holding. "Units" after a number counts
    // goods as often, so only a label says it is a holding.
    if (word == "shares" || word == "\xD0\xB0\xD0\xBA\xD1\x86\xD0\xB8\xD0\xB9" || word == "\xD0\xB0\xD0\xBA\xD1\x86\xD0\xB8\xD0\xB8"
        || word == "\xD0\xBF\xD0\xB0\xD0\xB5\xD0\xB2" || word == "\xD0\xBF\xD0\xB0\xD1\x91\xD0\xB2")  // акций акции паев паёв
      return decided(NumberMeaning::Holding);
    std::size_t unit = index;
    if (is_cue(word, kTimeQualifiers) && index + 1 < after.size() && after[index + 1].kind == Lexeme::Kind::Word)
      unit = index + 1;
    if (is_cue(after[unit].text, kTimeUnits) || is_cue(word, kCountUnits)) return decided(NumberMeaning::Generic);
    return {};
  }
  if (next.kind != Lexeme::Kind::Mark) return {};
  if (next.text == "%" || next.text == "\xE2\x80\xB0" || next.text == "\xC3\x97") return as_ratio();
  if (is_currency_mark(next)) return decided(NumberMeaning::Money);
  // "12-month", "2-й", and a statute's number, "152-ФЗ".
  if (next.text == "-" && glued && index + 1 < after.size() && after[index + 1].start == next.end
      && after[index + 1].kind == Lexeme::Kind::Word
      && (is_cue(after[index + 1].text, kTimeUnits) || is_cue(after[index + 1].text, kOrdinalEndings)
          || is_cue(after[index + 1].text, kStatuteSuffixes)))
    return decided(NumberMeaning::Generic);
  if (next.text == "(") return bracket_cue(after, index);
  return {};
}

// "р/с", "к/с", "л/с" (a Russian bank's settlement, correspondent and personal
// accounts) and "a/c", right before a number, a colon after them or not.
bool account_abbreviation_before(const std::vector<Lexeme> &before) {
  std::size_t count = before.size();
  if (count > 0 && before[count - 1].mark(":")) --count;
  if (count < 3) return false;
  const Lexeme &last = before[count - 1];
  const Lexeme &first = before[count - 3];
  return (last.word("с") || last.word("c")) && before[count - 2].mark("/")
      && (first.word("р") || first.word("к") || first.word("л") || first.word("a"));
}

// The label a number is given as the value of - "Purchase price: 12", "Gross
// margin = 0.64", "Итого — 19052" - read back from the mark to where its
// clause begins, eight words at most.
NumberMeaning label_pair(const std::vector<Lexeme> &before, std::size_t numberStart) {
  if (before.empty()) return NumberMeaning::Unknown;
  const Lexeme &mark = before.back();
  // A dash sets a label apart only with a space on each side: "Итого — 19052",
  // never the range in "16–18.05".
  const bool dash = mark.mark("-") || mark.mark("\xE2\x80\x94") || mark.mark("\xE2\x80\x93");
  const bool spaced = before.size() >= 2 && before[before.size() - 2].end < mark.start && mark.end < numberStart;
  if (!(mark.mark(":") || mark.mark("=") || mark.mark("|") || (dash && spaced))) return NumberMeaning::Unknown;
  std::size_t from = before.size() - 1;
  for (std::size_t taken = 0; from > 0 && taken < 8; ++taken) {
    const Lexeme &item = before[from - 1];
    if (ends_clause(before, from - 1) || item.mark(":") || item.mark("=") || item.mark("|")) break;
    --from;
  }
  return label_meaning(before, from, before.size() - 1);
}

// Words that stand between a noun and the number it is the amount of: "price
// of 12", "the fee is 12", "стоимость составляет 12", "не более 12".
constexpr std::string_view kQuantityLinks[] = {
    "of", "is", "was", "are", "were", "be", "been", "being", "shall", "will", "would", "should", "may", "must",
    "equal", "equals", "to", "at", "in", "the", "a", "an", "not", "less", "more", "than", "no", "up", "by", "for",
    "with", "and", "or", "amounting", "totaling", "totalling", "exceed", "exceeds", "exceeding", "over", "under",
    "approximately", "about", "around", "only", "its", "their", "set", "fixed", "agreed", "per",
    "в", "во", "на", "по", "до", "от", "из", "за", "с", "со", "к", "не", "ни", "менее", "более", "свыше", "около",
    "примерно", "составля*", "составил*", "составит", "равн*", "это", "его", "ее", "их", "и", "или", "а", "также",
};

// The noun a number is the amount of, earlier in its clause: "purchase price
// of 12", "the purchase price for the Shares is 12", "стоимость услуг
// составляет 12", "в размере 12". A noun right before a number is what the
// number belongs to - "file 20260", "убытки от инцидента 18.05" (the incident's
// date) - so the word right before it must be a linking word or the cue
// itself, and past a linking word one other word at most may stand between.
NumberMeaning clause_cue(const std::vector<Lexeme> &before) {
  std::size_t others = 0;
  bool linked = false;
  for (std::size_t index = before.size(); index > 0; --index) {
    const Lexeme &item = before[index - 1];
    if (ends_clause(before, index - 1) || item.kind == Lexeme::Kind::Number) break;
    if (is_currency_mark(item)) return NumberMeaning::Money;
    if (item.kind != Lexeme::Kind::Word) continue;
    if (is_cue(item.text, kMoneyWords) || is_cue(item.text, kCurrencyWords) || is_cue(item.text, kCurrencyCodes))
      return NumberMeaning::Money;
    if (is_cue(item.text, kIdentifierWords)) return NumberMeaning::Identifier;
    if (is_cue(item.text, kQuantityLinks)) {
      linked = true;
      continue;
    }
    if (!linked || ++others > 1) break;
  }
  return NumberMeaning::Unknown;
}

// A list's or a numbered clause's own number at the start of its line: "1.",
// "2)", "(3)", "8.1.", "## 3.".
bool is_list_number(const std::string &text, std::size_t windowStart, std::size_t start,
                    const std::vector<Lexeme> &after, std::size_t end) {
  // A clause numbered in running text: "…/2026. 1. Экспедитор несёт…", "; 2) the
  // Vendor…" - after a sentence's end, before its own full stop and a capital.
  std::size_t before = start;
  while (before > windowStart && (text[before - 1] == ' ' || text[before - 1] == '\t')) --before;
  if (before < start && before > windowStart && (text[before - 1] == '.' || text[before - 1] == ';')
      && after.size() >= 2 && after[0].start == end && (after[0].mark(".") || after[0].mark(")"))
      && after[1].kind == Lexeme::Kind::Word && after[1].start > after[0].end
      && is_upper(code_point_at(text, after[1].start)))
    return true;
  for (std::size_t cursor = windowStart; cursor < start; ++cursor) {
    const char character = text[cursor];
    if (character == ' ' || character == '\t' || character == '-' || character == '*' || character == '#'
        || character == '>' || character == '(')
      continue;
    if (text.compare(cursor, 3, "\xE2\x80\xA2") == 0) {
      cursor += 2;
      continue;
    }
    return false;
  }
  if (after.empty() || after.front().start != end) return false;
  // "(3)" numbers a list; "(3,065.00)" and "(1 200 000)" are an accountant's
  // negative amounts, which no list is numbered with.
  if (after.front().mark(")")) {
    const std::string_view number(text.data() + start, end - start);
    return number.size() <= 3 && number.find_first_of(", .") == std::string_view::npos;
  }
  return after.front().mark(".") && (after.size() == 1 || after[1].start > after.front().end);
}

NumberMeaning meaning_of(const std::string &text, std::size_t start, std::size_t end, LineReading &line, int depth);

// A number in running text, or in a cell of words: judged by the words beside
// it within its line or its cell, text[windowStart, windowEnd).
NumberMeaning prose_meaning(const std::string &text, std::size_t start, std::size_t end, std::size_t windowStart,
                            std::size_t windowEnd, std::uint64_t value, LineReading &line, int depth) {
  constexpr std::size_t kLookBack = 240;
  constexpr std::size_t kLookAhead = 160;
  const std::vector<Lexeme> before = lexemes(text, start > windowStart + kLookBack ? start - kLookBack : windowStart, start);
  const std::vector<Lexeme> after = lexemes(text, end, std::min(windowEnd, end + kLookAhead));
  const std::size_t count = before.size();
  const auto back = [&](std::size_t offset) -> const Lexeme * {
    return offset < count ? &before[count - 1 - offset] : nullptr;
  };
  const auto wordAt = [&](std::size_t offset, const auto &cues) {
    const Lexeme *item = back(offset);
    return item != nullptr && item->kind == Lexeme::Kind::Word && is_cue(item->text, cues);
  };

  if (is_list_number(text, windowStart, start, after, end)) return NumberMeaning::Generic;

  // A place in a document, whatever is written after its number: "Section 12",
  // "Section 9B", "Article 12(a)", "п. 9.4", "§ 5".
  if (const Lexeme *previous = back(0)) {
    if (previous->mark("\xC2\xA7") || previous->mark("\xC2\xB6") || wordAt(0, kStructureWords)
        || (previous->mark(".") && wordAt(1, kStructureWords) && back(1)->end == previous->start))
      return NumberMeaning::Generic;
  }

  const bool bracketed = back(0) != nullptr && back(0)->mark("(") && back(0)->end == start;
  const AfterCue cue = after_cue(after, end, bracketed, value);
  if (cue.ratio) {
    // A percentage is a financial ratio only as the value of a financial
    // label ("Gross margin: 64%"); in running text it is a threshold or a
    // share ("more than 50% of the equity"), which is how an agreement works.
    const NumberMeaning label = label_pair(before, start);
    return label == NumberMeaning::Money || label == NumberMeaning::FinancialRatio ? NumberMeaning::FinancialRatio
                                                                                   : NumberMeaning::Generic;
  }
  if (cue.decided) return cue.meaning;

  if (const Lexeme *previous = back(0)) {
    // A currency right before it: "$ 12", "US$12", "USD 12", "€12".
    if (is_currency_mark(*previous) || (previous->kind == Lexeme::Kind::Word && is_cue(previous->text, kCurrencyCodes)))
      return NumberMeaning::Money;
    // A number's sign - "№ 12", "No. 12", "# 12": the number of an identifier,
    // unless it numbers a part of a document ("Приложение № 2", "Section No. 5").
    std::size_t sign = count;
    if (previous->mark("\xE2\x84\x96") || previous->mark("#")) sign = 0;
    else if (previous->mark(".") && wordAt(1, kNumberSigns) && back(1)->end == previous->start) sign = 1;
    else if (previous->word("no") && text[previous->start] == 'N') sign = 0;
    if (sign < count) return wordAt(sign + 1, kStructureWords) ? NumberMeaning::Generic : NumberMeaning::Identifier;
    // An identifier named right before it: "ИНН 7710482917", "Account suffix
    // 12", "р/с 40702810400020004817".
    if (wordAt(0, kIdentifierWords)) return NumberMeaning::Identifier;
    if ((previous->mark(":") || previous->mark(".") || previous->mark("-")) && wordAt(1, kIdentifierWords))
      return NumberMeaning::Identifier;
    if (account_abbreviation_before(before)) return NumberMeaning::Identifier;
    // The year or the day of a date: "March 2026", "November 30, 2026", "FY
    // 2026", "H1 2026".
    if (wordAt(0, kMonthWords)) return NumberMeaning::DatePart;
    if (previous->mark(",") && back(1) != nullptr && back(1)->kind == Lexeme::Kind::Number && wordAt(2, kMonthWords))
      return NumberMeaning::DatePart;
    if (wordAt(0, kPeriodWords)) return NumberMeaning::DatePart;
    if (previous->word("year")) return value >= 1000 ? NumberMeaning::DatePart : NumberMeaning::Generic;
    // A year after the word that places something in time: "Revenue in 2024
    // was $12.5 million" names the year, never an amount of 2,024 (a verifier's
    // run, 25 Sep 2026, masked it as money).
    if (end == start + 4 && value >= 1900 && value <= 2199
        && (previous->word("in") || previous->word("since") || previous->word("during") || previous->word("until")
            || previous->word("till") || previous->word("through") || previous->word("before") || previous->word("after")))
      return NumberMeaning::DatePart;
    if (previous->kind == Lexeme::Kind::Number && back(1) != nullptr && (back(1)->word("h") || back(1)->word("q"))
        && back(1)->end == previous->start)
      return NumberMeaning::DatePart;
    // A plain scalar: "a probability of 0.64", "коэффициент 1,5".
    if (wordAt(0, kScalarWords)) return NumberMeaning::Generic;
    if ((previous->mark(":") || previous->mark("=") || wordAt(0, kScalarLinks)) && wordAt(1, kScalarWords))
      return NumberMeaning::Generic;
    // One of a run of numbers of one kind: "Sections 5 and 12", "rows 1-12 of
    // 17", "пп. 9.4 и 9.5", "fees of 12 and 15".
    const bool connector = wordAt(0, kNumberConnectors) || previous->mark("-") || previous->mark(",")
        || previous->mark("&") || previous->mark("\xE2\x80\x93") || previous->mark("\xE2\x80\x94");
    if (depth < 3 && connector && back(1) != nullptr && back(1)->kind == Lexeme::Kind::Number) {
      const NumberMeaning run = meaning_of(text, back(1)->start, back(1)->end, line, depth + 1);
      if (run == NumberMeaning::Generic || run == NumberMeaning::Money) return run;
    }
    // A group of an identifier written in parts: "паспорт 45 18".
    if (depth < 3 && previous->kind == Lexeme::Kind::Number && start == previous->end + 1 && text[previous->end] == ' '
        && meaning_of(text, previous->start, previous->end, line, depth + 1) == NumberMeaning::Identifier)
      return NumberMeaning::Identifier;
  }
  // A label it is the value of: "Purchase price: 12", "Итого — 19052".
  const NumberMeaning label = label_pair(before, start);
  if (label != NumberMeaning::Unknown) return label;
  // Money or an identifier named earlier in its clause.
  return clause_cue(before);
}

// A number standing as a cell's value in a row of the workbook reading
// ("Row 15: A15 = Purchase price ($m) | B15 = 12 (shown in $)"): judged by its
// note, the row's label - its first cell of words before this one - and the
// header of its column, from the header row its passage opens with.
NumberMeaning cell_meaning(const std::string &text, std::size_t end, std::uint64_t value, LineReading &line,
                           std::size_t cellIndex) {
  const SheetCell &cell = line.cells[cellIndex];
  const std::size_t formula = std::string_view(text).substr(end, cell.end - end).find(" {=");
  const std::vector<Lexeme> after =
      lexemes(text, end, formula == std::string_view::npos ? cell.end : end + formula);
  const AfterCue cue = after_cue(after, end, false, value);
  if (cue.decided && !cue.ratio) return cue.meaning;
  NumberMeaning row = NumberMeaning::Unknown;
  for (std::size_t index = 0; index < cellIndex; ++index) {
    const SheetCell &other = line.cells[index];
    const std::size_t textEnd = cell_text_end(text, other);
    if (holds_words(text, other.value_start, textEnd, 3)) {
      row = label_meaning(text, other.value_start, textEnd);
      break;
    }
  }
  NumberMeaning column = NumberMeaning::Unknown;
  if (!cell.column.empty()) {
    for (const HeaderLabel &label : line.passage_header_of(text)) {
      if (label.column != cell.column) continue;
      column = label_meaning(text, label.start, label.end);
      break;
    }
  }
  const auto either = [&](NumberMeaning wanted) { return row == wanted || column == wanted; };
  if (cue.ratio)
    return either(NumberMeaning::Money) || either(NumberMeaning::FinancialRatio) ? NumberMeaning::FinancialRatio
                                                                                 : NumberMeaning::Generic;
  if (either(NumberMeaning::Money)) return NumberMeaning::Money;
  if (either(NumberMeaning::FinancialRatio)) return NumberMeaning::FinancialRatio;
  if (either(NumberMeaning::Identifier)) return NumberMeaning::Identifier;
  if (either(NumberMeaning::Holding)) return NumberMeaning::Holding;
  if (either(NumberMeaning::Generic)) return NumberMeaning::Generic;
  return NumberMeaning::Unknown;
}

// A table as a document's reading writes one (DocxExtractor, the CSV and TSV
// readings): one line per row, its cells joined by tabs. The cells of the line
// text[lineStart, lineEnd), as where each begins and ends.
// Whether a line is a row of a table its reading writes with bars - "Seller |
// Shares | Taxpayer ID", a markdown table - rather than a workbook reading's
// "Row 4: A4 = … | B4 = …", whose cells are read their own way.
bool bar_table_line(const std::string &text, std::size_t lineStart, std::size_t lineEnd) {
  const std::string_view line(text.data() + lineStart, lineEnd - lineStart);
  if (line.find(" | ") == std::string_view::npos && line.find("| ") != 0) return false;
  return row_cells_start(text, lineStart, lineEnd) == std::string::npos && !opens_passage(line);
}

std::vector<std::pair<std::size_t, std::size_t>> tab_cells(const std::string &text, std::size_t lineStart,
                                                           std::size_t lineEnd) {
  std::vector<std::pair<std::size_t, std::size_t>> cells;
  std::size_t cellStart = lineStart;
  const bool bars = std::string_view(text.data() + lineStart, lineEnd - lineStart).find('\t') == std::string_view::npos
      && bar_table_line(text, lineStart, lineEnd);
  for (std::size_t cursor = lineStart; cursor <= lineEnd; ++cursor) {
    if (cursor < lineEnd && text[cursor] != '\t' && !(bars && text[cursor] == '|')) continue;
    cells.emplace_back(cellStart, cursor);
    cellStart = cursor + 1;
  }
  return cells;
}

// Whether a cell holds nothing but spaces.
bool blank_cell(const std::string &text, const std::pair<std::size_t, std::size_t> &cell) {
  for (std::size_t cursor = cell.first; cursor < cell.second; ++cursor)
    if (text[cursor] != ' ' && text[cursor] != '\r') return false;
  return true;
}

// A header that numbers the rows: "№", "No.", "#", "№ п/п", "N".
bool numbers_the_rows(const std::string &text, const std::pair<std::size_t, std::size_t> &header) {
  const std::vector<Lexeme> items = lexemes(text, header.first, header.second);
  if (items.empty()) return false;
  for (const Lexeme &item : items) {
    if (item.mark("\xE2\x84\x96") || item.mark("#") || item.mark(".") || item.mark("/")) continue;
    if (item.kind == Lexeme::Kind::Word && (item.text == "no" || item.text == "n" || item.text == "nr" || item.text == "п"
                                            || item.text == "пп" || item.text == "num"))
      continue;
    return false;
  }
  return true;
}

// The header of the table a tab-separated line belongs to: its first row, when
// that row is a header - two or more cells, and every cell that holds anything
// holds words ("Наименование | Кол-во | Итого, ₽", "Item | Amount ($)"). Read
// back line by line while the lines are the table's rows; nothing for the
// header row itself, a line of one cell, or a table whose first row is data.
std::optional<std::pair<std::size_t, std::size_t>> table_header_line(const std::string &text, std::size_t lineStart,
                                                                      std::size_t lineEnd) {
  const std::string_view line(text.data() + lineStart, lineEnd - lineStart);
  const bool tabs = line.find('\t') != std::string_view::npos;
  if (!tabs && !bar_table_line(text, lineStart, lineEnd)) return std::nullopt;
  const std::size_t floor = lineStart > kPassageReach ? lineStart - kPassageReach : 0;
  std::size_t first = lineStart;
  while (first > floor && first >= 2) {
    const std::size_t previousEnd = first - 1;
    const std::size_t previousStart = line_start_of(text, previousEnd);
    if (previousStart < floor || previousStart >= previousEnd) break;
    const std::string_view previous(text.data() + previousStart, previousEnd - previousStart);
    if (tabs ? previous.find('\t') == std::string_view::npos : !bar_table_line(text, previousStart, previousEnd)) break;
    first = previousStart;
  }
  if (first == lineStart) return std::nullopt;
  const std::size_t firstEnd = line_end_of(text, first);
  std::size_t filled = 0;
  for (const auto &cell : tab_cells(text, first, firstEnd)) {
    if (blank_cell(text, cell)) continue;
    if (!holds_words(text, cell.first, cell.second, 1) && !numbers_the_rows(text, cell)) return std::nullopt;
    ++filled;
  }
  if (filled < 2) return std::nullopt;
  return std::make_pair(first, firstEnd);
}

// Where the cell holding `position` of a tab-separated table row is written,
// and its column's header cell - both as where their text begins and ends.
struct TableCell {
  std::size_t start = 0;
  std::size_t end = 0;
  std::size_t index = 0;
  std::size_t line_start = 0;
  std::pair<std::size_t, std::size_t> header;
};

std::optional<TableCell> table_cell_at(const std::string &text, std::size_t position) {
  const std::size_t lineStart = line_start_of(text, position);
  const std::size_t lineEnd = line_end_of(text, position);
  const auto header = table_header_line(text, lineStart, lineEnd);
  if (!header) return std::nullopt;
  const std::vector<std::pair<std::size_t, std::size_t>> cells = tab_cells(text, lineStart, lineEnd);
  if (cells.size() < 2) return std::nullopt;
  const std::vector<std::pair<std::size_t, std::size_t>> headers = tab_cells(text, header->first, header->second);
  for (std::size_t index = 0; index < cells.size(); ++index) {
    if (position < cells[index].first || position >= cells[index].second) continue;
    if (index >= headers.size() || blank_cell(text, headers[index])) return std::nullopt;
    return TableCell{cells[index].first, cells[index].second, index, lineStart, headers[index]};
  }
  return std::nullopt;
}

// Whether a number is its row's own number in the table's first column, the
// cell holding it alone: "1", "2." under "Payment" count the instalments,
// whatever the header's word - the first row after the header is 1, the next 2.
// Under a header that says "Payment" they were once masked as amounts.
bool row_number_cell(const std::string &text, const TableCell &cell, std::size_t start, std::size_t end,
                     std::uint64_t value) {
  if (cell.index != 0 || value == 0) return false;
  std::size_t from = cell.start;
  std::size_t to = cell.end;
  while (from < to && text[from] == ' ') ++from;
  while (to > from && (text[to - 1] == ' ' || text[to - 1] == '\r')) --to;
  if (to > from && (text[to - 1] == '.' || text[to - 1] == ')')) --to;
  if (from != start || to != end) return false;
  std::uint64_t row = 0;
  for (std::size_t line = line_end_of(text, cell.header.first); line < cell.line_start && line < text.size();
       line = line_end_of(text, line + 1))
    ++row;
  return row == value;
}

// A number in a cell of such a table: judged by the words right after it in
// its cell, by the words of its own cell, then by its column's header - what a
// document's table says of every figure under it: "18 400 000" under «Итого,
// ₽» is money, "3" under «Кол-во» a count however its row is labelled, "1"
// under "№" the row's own number - and only under a header that says nothing,
// by its row's label (the row's first cell of words before it). Nothing when
// the table has no header row: the line is read as the words it is.
std::optional<NumberMeaning> table_cell_meaning(const std::string &text, std::size_t start, std::size_t end,
                                                std::size_t last, std::uint64_t value, LineReading &line, int depth) {
  const std::optional<TableCell> cell = table_cell_at(text, start);
  if (!cell) return std::nullopt;
  if (numbers_the_rows(text, cell->header)) return NumberMeaning::Generic;
  if (row_number_cell(text, *cell, start, end, value)) return NumberMeaning::Generic;
  const std::vector<Lexeme> after = lexemes(text, last, cell->end);
  const AfterCue cue = after_cue(after, last, false, value);
  if (cue.decided && !cue.ratio) return cue.meaning;
  NumberMeaning row = NumberMeaning::Unknown;
  const std::vector<std::pair<std::size_t, std::size_t>> cells =
      tab_cells(text, cell->line_start, line_end_of(text, cell->line_start));
  for (std::size_t index = 0; index < cell->index && index < cells.size(); ++index) {
    if (!holds_words(text, cells[index].first, cells[index].second, 3)) continue;
    row = label_meaning(text, cells[index].first, cells[index].second);
    break;
  }
  const NumberMeaning column = label_meaning(text, cell->header.first, cell->header.second);
  const auto either = [&](NumberMeaning wanted) { return row == wanted || column == wanted; };
  if (cue.ratio)
    return either(NumberMeaning::Money) || either(NumberMeaning::FinancialRatio) ? NumberMeaning::FinancialRatio
                                                                                 : NumberMeaning::Generic;
  const NumberMeaning own = prose_meaning(text, start, end, cell->start, cell->end, value, line, depth);
  if (own != NumberMeaning::Unknown) return own;
  return column != NumberMeaning::Unknown ? column : row;
}

NumberMeaning meaning_of(const std::string &text, std::size_t start, std::size_t end, LineReading &line, int depth) {
  std::size_t first = start;
  while (first < end && !is_ascii_digit(text[first])) ++first;
  if (first == end) return NumberMeaning::Unknown;
  std::size_t last = end;
  while (last > first && !is_ascii_digit(text[last - 1])) --last;
  const std::string_view core(text.data() + first, last - first);
  // Zero is nobody's amount and nobody's number.
  if (std::none_of(core.begin(), core.end(), [](char character) { return character >= '1' && character <= '9'; }))
    return NumberMeaning::Generic;
  // A phone's country code, which the phone's own pattern protects.
  if (first > 0 && text[first - 1] == '+') return NumberMeaning::Unknown;
  // Part of a cell reference - "B10", "E80", "$B$3": a place, never a value.
  // A number glued to any other code ("MR-20417", "СП-087") is that code's,
  // which the code rules protect as a whole (find_labelled_codes()).
  if (glued_before(text, first)) return cell_reference_shape(text, first) ? NumberMeaning::Generic : NumberMeaning::Unknown;
  const std::size_t lineStart = line_start_of(text, first);
  if (inside_formula(text, lineStart, first)) return NumberMeaning::Generic;
  // "от 02.07", "on 7/2": a day written without its year.
  if (yearless_day(text, first, last - first)) return NumberMeaning::DatePart;
  // «п/п № 3318»: a payment order's number, not a list's row.
  if (after_number_sign(text, first)) {
    std::size_t cursor = first;
    while (cursor > 0 && (text[cursor - 1] == ' ' || (cursor >= 2 && text.compare(cursor - 2, 2, "\xC2\xA0") == 0)))
      cursor -= text[cursor - 1] == ' ' ? 1 : 2;
    if (cursor >= 3 && text.compare(cursor - 3, 3, "\xE2\x84\x96") == 0) {
      cursor -= 3;
      while (cursor > 0 && text[cursor - 1] == ' ') --cursor;
      static const std::string paymentOrder = "\xD0\xBF/\xD0\xBF";  // п/п
      if (cursor >= paymentOrder.size() && text.compare(cursor - paymentOrder.size(), paymentOrder.size(), paymentOrder) == 0
          && (cursor == paymentOrder.size() || !is_letter_or_mark(code_point_before(text, cursor - paymentOrder.size()).second)))
        return NumberMeaning::Identifier;
    }
  }
  switch (number_shape(core)) {
    case NumberShape::Date:
      // "Договор № 15/03/2026", "Invoice No. 2026-03-15": a document's number
      // written like a date is that number, and it comes back as written.
      if (after_number_sign(text, first)) return NumberMeaning::Identifier;
      return NumberMeaning::DatePart;
    case NumberShape::Time:
      return NumberMeaning::DatePart;
    case NumberShape::Clause:
      // "№ 73.5474.935": after its sign a number written like a clause's is a
      // document's number; "п. 9.4" and "Section 18.2" stay structure.
      if (after_number_sign(text, first)) return NumberMeaning::Identifier;
      return NumberMeaning::Generic;
    default:
      break;
  }
  const std::uint64_t value = leading_value(core);
  const std::size_t lineEnd = line_end_of(text, last);
  line.read(text, lineStart, lineEnd);
  std::size_t windowStart = lineStart;
  std::size_t windowEnd = lineEnd;
  if (line.cells_start != std::string::npos) {
    // "Row 12": the row's own number.
    if (first < line.cells_start) return NumberMeaning::Generic;
    for (std::size_t index = 0; index < line.cells.size(); ++index) {
      const SheetCell &cell = line.cells[index];
      if (first < cell.start || first >= cell.end) continue;
      if (first < cell.value_start) return NumberMeaning::Generic;
      std::size_t valueStart = cell.value_start;
      while (valueStart < first && (text[valueStart] == ' ' || text[valueStart] == '-' || text[valueStart] == '('))
        ++valueStart;
      if (valueStart == first) return cell_meaning(text, last, value, line, index);
      windowStart = cell.value_start;
      windowEnd = cell.end;
      break;
    }
  } else if (const std::optional<NumberMeaning> table = table_cell_meaning(text, first, last, last, value, line, depth)) {
    return *table;
  }
  return prose_meaning(text, start, end, windowStart, windowEnd, value, line, depth);
}

// A sensitive number's whole writing: its sign, a currency sign or code right
// before it, and the scale, the number words in brackets and the currency
// after it - "-2619052", "$ 12", "12 million", "50 000 000 (пятидесяти
// миллионов) рублей", "3,45 млн ₽".
std::pair<std::size_t, std::size_t> amount_bounds(const std::string &text, std::size_t start, std::size_t end) {
  if (start > 0 && text[start - 1] == '-' && (start < 2 || !is_word(code_point_before(text, start - 1).second)))
    --start;
  const std::size_t lineStart = line_start_of(text, start);
  const std::vector<Lexeme> before = lexemes(text, start > lineStart + 16 ? start - 16 : lineStart, start);
  if (!before.empty()) {
    const Lexeme &previous = before.back();
    if ((is_currency_mark(previous) || (previous.kind == Lexeme::Kind::Word && is_cue(previous.text, kCurrencyCodes)))
        && start - previous.end <= 1)
      start = previous.start;
  }
  const std::vector<Lexeme> after = lexemes(text, end, std::min(line_end_of(text, end), end + 160));
  const auto close = [&](const Lexeme &item) {
    return item.start - end <= 3 && std::all_of(text.begin() + static_cast<std::ptrdiff_t>(end),
                                                text.begin() + static_cast<std::ptrdiff_t>(item.start),
                                                [](char character) { return character == ' ' || (static_cast<unsigned char>(character) >= 0x80); });
  };
  std::size_t index = 0;
  for (int taken = 0; taken < 3 && index < after.size(); ++taken) {
    const Lexeme &next = after[index];
    if (!close(next)) break;
    const bool glued = next.start == end;
    if (next.kind == Lexeme::Kind::Word
        && (is_cue(next.text, kScaleWords) || is_cue(next.text, kCurrencyWords) || is_cue(next.text, kCurrencyCodes)
            || (glued && is_cue(next.text, kScaleSuffixes)) || next.text == "р")) {
      if (next.text == "р" && !(index + 1 < after.size() && after[index + 1].mark(".") && after[index + 1].start == next.end))
        break;
      end = next.end;
      ++index;
      // The full stop of "руб." stays outside: it may also end the sentence,
      // and a symbol that swallowed it comes back with the model's own after it.
      if (index < after.size() && after[index].mark(".") && after[index].start == end) break;
      continue;
    }
    if (is_currency_mark(next)) {
      end = next.end;
      ++index;
      continue;
    }
    if (next.mark("(")) {
      std::size_t closing = index + 1;
      bool words = false;
      while (closing < after.size() && after[closing].kind == Lexeme::Kind::Word
             && (is_cue(after[closing].text, kNumberWords) || is_cue(after[closing].text, kCurrencyWords))) {
        ++closing;
        words = true;
      }
      if (!words || closing >= after.size() || !after[closing].mark(")")) break;
      end = after[closing].end;
      index = closing + 1;
      continue;
    }
    break;
  }
  return {start, end};
}

// Whether the code at text[start, end) is labelled an identifier: by the words
// right before it - "employee ID MR-20417", "staff number EMP-00931",
// "табельный номер ТН-118", "policy No. PL-1234" - or, as a cell's whole value
// in a row of the workbook reading, by its row's label or its column's header
// ("A4 = Employee ID | B4 = MR-20417").
bool labelled_as_identifier(const std::string &text, std::size_t start, std::size_t end) {
  const std::size_t lineStart = line_start_of(text, start);
  const std::size_t lineEnd = line_end_of(text, start);
  LineReading line;
  line.read(text, lineStart, lineEnd);
  if (line.cells_start != std::string::npos) {
    for (std::size_t index = 0; index < line.cells.size(); ++index) {
      const SheetCell &cell = line.cells[index];
      if (start < cell.start || start >= cell.end) continue;
      if (cell.value_start != start || cell_text_end(text, cell) != end) break;
      for (std::size_t other = 0; other < index; ++other) {
        const std::size_t textEnd = cell_text_end(text, line.cells[other]);
        if (!holds_words(text, line.cells[other].value_start, textEnd, 3)) continue;
        if (label_meaning(text, line.cells[other].value_start, textEnd) == NumberMeaning::Identifier) return true;
        break;
      }
      if (!cell.column.empty())
        for (const HeaderLabel &label : line.passage_header_of(text))
          if (label.column == cell.column && label_meaning(text, label.start, label.end) == NumberMeaning::Identifier)
            return true;
      return false;
    }
  }
  const std::vector<Lexeme> before = lexemes(text, start > lineStart + 80 ? start - 80 : lineStart, start);
  std::size_t index = before.size();
  while (index > 0 && (before[index - 1].mark(":") || before[index - 1].mark("(") || before[index - 1].mark("=")
                       || before[index - 1].mark("\xE2\x80\x94") || before[index - 1].mark("-")))
    --index;
  if (index == 0) return false;
  const Lexeme &last = before[index - 1];
  if (last.kind == Lexeme::Kind::Word && is_cue(last.text, kIdentifierWords)) return true;
  // A sign after the record: "employee no. MR-1", "staff number EMP-9", "policy № 12".
  if (last.mark("\xE2\x84\x96") || last.mark("#")) --index;
  else if (last.mark(".") && index >= 2 && before[index - 2].kind == Lexeme::Kind::Word
           && is_cue(before[index - 2].text, kCodeSigns))
    index -= 2;
  else if (last.kind == Lexeme::Kind::Word && is_cue(last.text, kCodeSigns)) --index;
  else return false;
  return index > 0 && before[index - 1].kind == Lexeme::Kind::Word && is_cue(before[index - 1].text, kRecordWords);
}

// A record's code - capitals, then groups of figures after a hyphen or a slash:
// "MR-20417", "EMP-00931", "ТН-118" - wherever the text labels it an
// identifier (labelled_as_identifier()), whether or not the semantic model
// tagged it. An unseen matter (24 Sep 2026) sent an employee's ID readable in
// a workbook and in a complaint's prose. A cell reference - letters and figures
// with no hyphen, "B10" - is never one.
std::vector<Match> find_labelled_codes(const std::string &text) {
  const auto digitsAt = [&](std::size_t position) {
    std::size_t end = position;
    while (end < text.size() && is_ascii_digit(text[end])) ++end;
    return end - position;
  };
  return find_at_word_starts(text, to_string(Category::Identifiers),
                             [&](std::size_t start) -> std::optional<std::pair<std::size_t, std::size_t>> {
    std::size_t cursor = start;
    std::size_t letters = 0;
    for (CodePoint point = code_point_at(text, cursor); is_upper(point) && letters <= 4;
         point = code_point_at(text, cursor)) {
      cursor += point.length;
      ++letters;
    }
    if (letters == 0 || letters > 4) return std::nullopt;
    int groups = 0;
    while (cursor < text.size() && (text[cursor] == '-' || text[cursor] == '/')) {
      const std::size_t digits = digitsAt(cursor + 1);
      if (digits == 0 || digits > 8) break;
      cursor += 1 + digits;
      ++groups;
    }
    if (groups == 0 || is_word(code_point_at(text, cursor))) return std::nullopt;
    if (!labelled_as_identifier(text, start, cursor)) return std::nullopt;
    return std::make_pair(start, cursor);
  });
}

// A labelled code of letters and figures mixed, in any order and any groups:
// "VIN 1HGCM82633A004352", "MBI 1EG4-TE5-MK73", "license D1234567", "Policy No.
// HO3-4455667" - whole, wherever the text labels it an identifier. The shapes
// above read capitals then figures; these went out whole or left "HO3-" beside
// a mask. At least five letters and figures, two of them figures, and never a
// word of letters alone.
std::vector<Match> find_labelled_alphanumeric_codes(const std::string &text) {
  return find_at_word_starts(text, to_string(Category::Identifiers),
                             [&](std::size_t start) -> std::optional<std::pair<std::size_t, std::size_t>> {
    std::size_t cursor = start;
    std::size_t characters = 0;
    std::size_t digits = 0;
    std::size_t letters = 0;
    for (;;) {
      const std::size_t runStart = cursor;
      while (cursor < text.size() && (is_ascii_digit(text[cursor]) || is_ascii_letter(text[cursor]))) {
        if (is_ascii_digit(text[cursor])) ++digits;
        else ++letters;
        ++cursor;
        ++characters;
      }
      if (cursor == runStart) return std::nullopt;
      if (cursor + 1 < text.size() && (text[cursor] == '-' || text[cursor] == '/')
          && (is_ascii_digit(text[cursor + 1]) || is_ascii_letter(text[cursor + 1]))) {
        ++cursor;
        continue;
      }
      break;
    }
    if (characters < 5 || digits < 2 || letters == 0 || is_word(code_point_at(text, cursor))) return std::nullopt;
    // Lower-case letters make a word ("2nd-floor", "covid19-safe"), not a code.
    for (std::size_t index = start; index < cursor; ++index)
      if (text[index] >= 'a' && text[index] <= 'z') return std::nullopt;
    if (!labelled_as_identifier(text, start, cursor)) return std::nullopt;
    return std::make_pair(start, cursor);
  });
}

}  // namespace

std::pair<std::size_t, std::size_t> quantity_span(const std::string &text, std::size_t start, std::size_t end);

namespace {

// Every number of `text` whose own context makes it money, a financial ratio
// or an identifier (number_meaning()), found however short it is and whether or
// not the semantic model tagged it: "B15 = 12 (shown in $)", "Gross margin |
// 0.64 (shown as a percentage)", "протокол № 7".
std::vector<Match> find_meaningful_numbers(const std::string &text, bool money, bool identifiers) {
  std::vector<Match> matches;
  if (!money && !identifiers) return matches;
  const std::string moneyLabel = to_string(Category::MonetaryAmounts);
  const std::string identifierLabel = to_string(Category::Identifiers);
  LineReading line;
  std::size_t position = 0;
  while (position < text.size()) {
    if (!is_ascii_digit(text[position])) {
      ++position;
      continue;
    }
    const auto [start, end] = whole_number_bounds(text, position, position + 1);
    position = std::max(end, position + 1);
    if (glued_before(text, start)) continue;
    const NumberMeaning meaning = meaning_of(text, start, end, line, 0);
    if (money && (meaning == NumberMeaning::Money || meaning == NumberMeaning::FinancialRatio)) {
      // A ratio keeps the percent it is written with: "18%" is the rate, and a
      // symbol for "18" alone would read as a bare number beside a sign.
      const auto [spanStart, spanEnd] =
          meaning == NumberMeaning::Money ? amount_bounds(text, start, end) : quantity_span(text, start, end);
      // A ratio is protected with money and typed as the rate it is.
      matches.push_back(Match{spanStart, spanEnd - spanStart,
          meaning == NumberMeaning::Money ? moneyLabel : std::string(percentage_label),
          text.substr(spanStart, spanEnd - spanStart)});
      position = std::max(position, spanEnd);
    } else if (money && meaning == NumberMeaning::Holding) {
      matches.push_back(Match{start, end - start, std::string(holding_label), text.substr(start, end - start)});
    } else if (identifiers && meaning == NumberMeaning::Identifier) {
      // After its sign a document's number is the whole code it is written
      // as: "Требование № 14/ИТ", "invoice No. 4471/B", "№ 12-3/7" - never its
      // first figures with the rest left readable.
      std::size_t codeEnd = end;
      if (after_number_sign(text, start)) {
        while (codeEnd + 1 < text.size() && (text[codeEnd] == '/' || text[codeEnd] == '-' || text[codeEnd] == '.')
               && is_ascii_digit(text[codeEnd + 1]) && digits_from(text, codeEnd + 1) <= 8)
          codeEnd += 1 + digits_from(text, codeEnd + 1);
        codeEnd = code_letter_suffix_end(text, codeEnd);
        if (is_word(code_point_at(text, codeEnd))) codeEnd = end;
      }
      position = std::max(position, codeEnd);
      // One identifier written in groups is one value: "паспорт 45 18".
      Match *previous = matches.empty() ? nullptr : &matches.back();
      if (previous != nullptr && previous->label == identifierLabel && previous->start + previous->length + 1 == start
          && text[start - 1] == ' ') {
        previous->length = codeEnd - previous->start;
        previous->value = text.substr(previous->start, previous->length);
      } else {
        matches.push_back(Match{start, codeEnd - start, identifierLabel, text.substr(start, codeEnd - start)});
      }
    }
  }
  return matches;
}

std::string to_lower_ascii(const std::string &value) {
  std::string result = value;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

std::vector<Match> find_custom_term_matches(const std::string &text, const std::string &term) {
  std::vector<Match> matches;
  const auto lower_text = to_lower_ascii(text);
  const auto lower_term = to_lower_ascii(term);

  std::size_t search_from = 0;
  std::size_t position;
  while ((position = lower_text.find(lower_term, search_from)) != std::string::npos) {
    matches.push_back(Match{position, term.size(), "custom_term", text.substr(position, term.size())});
    search_from = position + term.size();
  }
  return matches;
}

bool spans_overlap(const Match &a, const Match &b) {
  return a.start < b.start + b.length && b.start < a.start + a.length;
}

} // namespace

std::vector<Match> detect(const std::string &text, const Policy &policy) {
  if (text.empty()) return {};

  struct Candidate {
    Match match;
    int priority;
  };
  std::vector<Candidate> candidates;

  // Priority 0: custom terms always win an overlap with a built-in
  // category - see Policy::custom_terms. Gated on mode != Off same as the
  // built-in categories below, so turning Protected API off is unambiguous
  // (see policy.hpp).
  if (policy.mode != Mode::Off) {
    for (const auto &term : policy.custom_terms) {
      if (term.empty()) continue;
      for (auto &match : find_custom_term_matches(text, term)) {
        candidates.push_back(Candidate{std::move(match), 0});
      }
    }
  }

  // Priority 1: an address or a name. Active whenever protection is, like
  // the names Papinian NER finds (see policy.hpp), and ahead of the
  // built-in categories so a number inside an address never leaves the rest
  // of it readable.
  if (policy.mode != Mode::Off) {
    for (auto &match : find_shape_matches(text)) {
      candidates.push_back(Candidate{std::move(match), 1});
    }
  }

  // The numbers whose own context makes them money or an identifier, however
  // short: "B15 = 12 (shown in $)" under "Purchase price ($m)", "протокол № 7".
  const std::vector<Category> categories = effective_categories(policy);
  const auto active = [&](Category category) {
    return std::find(categories.begin(), categories.end(), category) != categories.end();
  };
  std::vector<Match> numbers =
      find_meaningful_numbers(text, active(Category::MonetaryAmounts), active(Category::Identifiers));
  // And a record's code its label makes an identifier: "employee ID MR-20417".
  if (active(Category::Identifiers)) {
    const std::vector<Match> codes = find_labelled_codes(text);
    numbers.insert(numbers.end(), codes.begin(), codes.end());
    const std::vector<Match> mixed = find_labelled_alphanumeric_codes(text);
    numbers.insert(numbers.end(), mixed.begin(), mixed.end());
  }

  int priority = 2;
  for (const auto category : categories) {
    for (auto &match : find_category_matches(text, category)) {
      // A number found glued to letters ("ИНН7710482917", "СП-087654") is
      // that whole code: a number is only ever matched again where it stands
      // whole (stands_as_whole_number()).
      if (!match.value.empty() && is_ascii_digit(match.value.front()) && glued_before(text, match.start)
          && text[match.start - 1] != '+') {
        const auto [start, end] = code_bounds(text, match.start, match.start + match.length);
        match.start = start;
        match.length = end - start;
        match.value = text.substr(start, end - start);
      }
      candidates.push_back(Candidate{std::move(match), priority});
    }
    for (const Match &match : numbers)
      if (match.label == to_string(category)
          || (category == Category::MonetaryAmounts && (match.label == percentage_label || match.label == holding_label)))
        candidates.push_back(Candidate{match, priority});
    priority++;
  }

  std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
    if (a.priority != b.priority) return a.priority < b.priority;
    if (a.match.start != b.match.start) return a.match.start < b.match.start;
    return a.match.length > b.match.length;
  });

  std::vector<Match> accepted;
  for (const auto &candidate : candidates) {
    const bool conflicts = std::any_of(accepted.begin(), accepted.end(),
                                       [&](const Match &existing) { return spans_overlap(candidate.match, existing); });
    if (!conflicts) {
      accepted.push_back(candidate.match);
    }
  }

  std::sort(accepted.begin(), accepted.end(), [](const Match &a, const Match &b) { return a.start < b.start; });
  return accepted;
}

std::pair<std::size_t, std::size_t> quantity_span(const std::string &text, std::size_t start, std::size_t end) {
  if (start >= end || end > text.size()) return {start, end};
  auto [spanStart, spanEnd] = amount_bounds(text, start, end);
  // A percent sign or word after the number: "3%", "0,1 %", "5 percent",
  // "0,2 процента". Only where nothing but a space stands between them.
  if (spanEnd == end) {
    std::size_t cursor = end;
    if (cursor < text.size() && text[cursor] == ' ') ++cursor;
    else if (text.compare(cursor, 2, "\xC2\xA0") == 0) cursor += 2;
    else if (text.compare(cursor, 3, "\xE2\x80\xAF") == 0 || text.compare(cursor, 3, "\xE2\x80\x89") == 0) cursor += 3;
    if (cursor < text.size() && text[cursor] == '%') {
      spanEnd = cursor + 1;
    } else {
      const std::vector<Lexeme> after = lexemes(text, end, std::min(line_end_of(text, end), end + 40));
      if (!after.empty() && after.front().start == cursor && after.front().kind == Lexeme::Kind::Word
          && is_percent_at(after, 0)) {
        spanEnd = after.front().end;
        // "per cent", "basis points": both words.
        if ((after.front().text == "per" || after.front().text == "basis") && after.size() > 1) spanEnd = after[1].end;
      }
    }
  }
  return {spanStart, spanEnd};
}

NumberMeaning number_meaning(const std::string &text, std::size_t start, std::size_t length) {
  if (length == 0 || start + length > text.size()) return NumberMeaning::Unknown;
  LineReading line;
  return meaning_of(text, start, start + length, line, 0);
}

bool stands_as_whole_number(const std::string &text, std::size_t start, std::size_t length) {
  const std::size_t end = start + length;
  if (length == 0 || end > text.size()) return false;
  if (start > 0 && is_ascii_digit(text[start - 1])) return false;
  if (end < text.size() && is_ascii_digit(text[end])) return false;
  if (is_ascii_digit(text[start])) {
    if (start >= 2 && joins_numbers(text[start - 1]) && is_ascii_digit(text[start - 2])) return false;
    if (grouped_with_digits_before(text, start)) return false;
    if (glued_before(text, start)) return false;
  }
  if (is_ascii_digit(text[end - 1])) {
    if (end + 1 < text.size() && joins_numbers(text[end]) && is_ascii_digit(text[end + 1])) return false;
    if (grouped_with_digits_after(text, end)) return false;
  }
  return true;
}

std::pair<std::size_t, std::size_t> detected_number_span(const std::string &text, std::size_t start,
                                                         std::size_t end) {
  const auto [numberStart, numberEnd] = whole_number_bounds(text, start, end);
  if (numberStart >= numberEnd || !is_ascii_digit(text[numberStart])) return {numberStart, numberEnd};
  if (numberStart > 0 && text[numberStart - 1] == '+') return {numberStart - 1, numberEnd};
  if (!glued_before(text, numberStart) || reads_as_cell_reference(text, numberStart)) return {numberStart, numberEnd};
  return code_bounds(text, numberStart, numberEnd);
}

std::optional<std::pair<std::size_t, std::size_t>> table_column_header(const std::string &text, std::size_t position) {
  if (position >= text.size()) return std::nullopt;
  const std::optional<TableCell> cell = table_cell_at(text, position);
  if (!cell) return std::nullopt;
  std::size_t from = cell->header.first;
  std::size_t to = cell->header.second;
  while (from < to && text[from] == ' ') ++from;
  while (to > from && (text[to - 1] == ' ' || text[to - 1] == '\r')) --to;
  return std::make_pair(from, to);
}

bool after_date_cue(const std::string &text, std::size_t start) {
  static const std::string_view cues[] = {"\xD0\xBE\xD1\x82", "\xD1\x81", "\xD0\xB4\xD0\xBE", "\xD0\xBF\xD0\xBE",
      "\xD0\xBF\xD0\xBE\xD0\xB7\xD0\xB4\xD0\xBD\xD0\xB5\xD0\xB5", "\xD0\xBD\xD0\xB0", "on", "by", "dated",
      "from", "until", "till", "effective", "of"};  // от с до по позднее на
  std::size_t cursor = std::min(start, text.size());
  while (cursor > 0 && text[cursor - 1] == ' ') --cursor;
  std::size_t from = cursor;
  while (from > 0) {
    const auto [previous, point] = code_point_before(text, from);
    if (!is_letter_or_mark(point)) break;
    from = previous;
  }
  if (from == cursor) return false;
  const std::string word = lowered_word(std::string_view(text.data() + from, cursor - from));
  if (std::find(std::begin(cues), std::end(cues), word) == std::end(cues)) return false;
  // "of" places a day only as "as of": "3/4 of the price" is a fraction.
  if (word == "of") {
    std::size_t before = from;
    while (before > 0 && text[before - 1] == ' ') --before;
    return before >= 2 && lowered_word(std::string_view(text.data() + before - 2, 2)) == "as"
        && (before == 2 || !std::isalpha(static_cast<unsigned char>(text[before - 3])));
  }
  return true;
}

std::optional<std::pair<int, int>> yearless_day(const std::string &text, std::size_t start, std::size_t length) {
  const std::size_t end = start + length;
  if (length < 3 || length > 5 || end > text.size()) return std::nullopt;
  const std::string_view written(text.data() + start, length);
  const std::size_t mark = written.find_first_of("./");
  if (mark == std::string_view::npos || mark == 0 || mark > 2 || written.find_first_of("./", mark + 1) != std::string_view::npos)
    return std::nullopt;
  const std::string_view first = written.substr(0, mark);
  const std::string_view second = written.substr(mark + 1);
  if (second.empty() || second.size() > 2) return std::nullopt;
  for (const char character : first) if (!is_ascii_digit(character)) return std::nullopt;
  for (const char character : second) if (!is_ascii_digit(character)) return std::nullopt;
  // Standing whole: no figure, full stop or slash of a longer number around it.
  if (start > 0 && (is_ascii_digit(text[start - 1]) || text[start - 1] == '.' || text[start - 1] == '/'
          || text[start - 1] == ',' || text[start - 1] == '-'))
    return std::nullopt;
  if (end < text.size() && (is_ascii_digit(text[end]) || text[end] == '/'
          || ((text[end] == '.' || text[end] == ',') && end + 1 < text.size() && is_ascii_digit(text[end + 1]))))
    return std::nullopt;
  if (!after_date_cue(text, start)) return std::nullopt;
  const int a = std::stoi(std::string(first));
  const int b = std::stoi(std::string(second));
  // Full stops read day first ("02.07" is 2 July), slashes month first.
  const bool dots = written[mark] == '.';
  if (dots && second.size() != 2) return std::nullopt;
  const int month = dots ? b : a;
  const int day = dots ? a : b;
  if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
  return std::make_pair(month, day);
}

bool after_number_sign(const std::string &text, std::size_t start) {
  std::size_t cursor = std::min(start, text.size());
  while (cursor > 0 && (text[cursor - 1] == ' ' || (cursor >= 2 && text.compare(cursor - 2, 2, "\xC2\xA0") == 0)))
    cursor -= text[cursor - 1] == ' ' ? 1 : 2;
  if (cursor >= 3 && text.compare(cursor - 3, 3, "\xE2\x84\x96") == 0) return true;
  if (cursor >= 1 && text[cursor - 1] == '#') return true;
  const bool stop = cursor >= 1 && text[cursor - 1] == '.';
  if (stop) --cursor;
  std::size_t from = cursor;
  while (from > 0 && std::isalpha(static_cast<unsigned char>(text[from - 1])) != 0) --from;
  if (from > 0 && (is_ascii_digit(text[from - 1]) || static_cast<unsigned char>(text[from - 1]) >= 0x80)) return false;
  const std::string word = text.substr(from, cursor - from);
  // "no" in lower case is the word ("no 2026 budget") unless its stop makes it the sign.
  return word == "No" || word == "NO" || word == "Nr" || word == "N" || word == "Nos" || (stop && (word == "no" || word == "nr"));
}

bool stands_as_cell_reference(const std::string &text, std::size_t start, std::size_t length) {
  const std::size_t end = start + length;
  if (length == 0 || end > text.size()) return false;
  std::size_t digits = start;
  while (digits < end && (text[digits] == '$' || is_ascii_upper(text[digits]))) ++digits;
  if (digits == start || digits == end || !is_ascii_digit(text[digits])) return false;
  for (std::size_t index = digits; index < end; ++index)
    if (!is_ascii_digit(text[index])) return false;
  return reads_as_cell_reference(text, digits);
}

bool names_only_structure(const std::string &text, std::size_t start, std::size_t length) {
  if (length == 0 || start + length > text.size()) return false;
  const std::vector<Lexeme> items = lexemes(text, start, start + length);
  LineReading line;
  bool number = false;
  bool statuteTitle = false;
  const auto letters = [](const std::string &word) {
    return static_cast<std::size_t>(std::count_if(word.begin(), word.end(), [](char byte) {
      return (static_cast<unsigned char>(byte) & 0xC0) != 0x80;
    }));
  };
  // A clause's sub-part in brackets: the "(b)" of "9.4(b)", the "(a)(ii)" of
  // "12(a)(ii)", the "(1)" of "Section 5(1)".
  const auto subPart = [&](std::size_t index) {
    return index > 0 && index + 1 < items.size() && items[index - 1].mark("(") && items[index + 1].mark(")")
        && letters(items[index].text) <= 4;
  };
  for (std::size_t index = 0; index < items.size(); ++index) {
    const Lexeme &item = items[index];
    switch (item.kind) {
      case Lexeme::Kind::Number:
        if (subPart(index)) break;
        if (meaning_of(text, item.start, item.end, line, 0) != NumberMeaning::Generic) return false;
        number = true;
        break;
      case Lexeme::Kind::Word:
        // A clause's letter, glued to its number: "Section 9A", "Раздел 9А", "12bis".
        if (index > 0 && items[index - 1].kind == Lexeme::Kind::Number && items[index - 1].end == item.start
            && letters(item.text) <= 3)
          break;
        if (subPart(index)) break;
        // A statute's title after its name: "статьёй 19 Закона о персональных
        // данных", "Article 6 of the Law on Personal Data" - public law.
        if (statuteTitle) break;
        if (index + 1 < items.size() && is_cue(item.text, kStatuteWords)
            && (items[index + 1].word("о") || items[index + 1].word("об") || items[index + 1].word("on")
                || items[index + 1].word("of"))) {
          statuteTitle = true;
          break;
        }
        if (!is_cue(item.text, kStructureWords) && !is_cue(item.text, kTimeUnits) && !is_cue(item.text, kTimeQualifiers)
            && !is_cue(item.text, kCountUnits) && !is_cue(item.text, kReferenceWords)
            && !is_cue(item.text, kNumberConnectors))
          return false;
        break;
      case Lexeme::Kind::Mark:
        if (!(item.mark(".") || item.mark(",") || item.mark("(") || item.mark(")") || item.mark(":") || item.mark("-")
              || item.mark("\xE2\x80\x93") || item.mark("\xC2\xA7") || item.mark("\xC2\xB6")))
          return false;
        break;
    }
  }
  return number;
}

} // namespace singularity::protected_api
