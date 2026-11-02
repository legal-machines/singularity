# Common words

## Summary

- **The list.** `english_common_words.txt` is a public-domain list of ordinary
  English words.
- **What it is for.** Papinian Guard uses it in one direction only: to stop
  treating a detected name as an ordinary word when the document itself writes
  it as one. It never decides alone that something is safe to send.
- **If it is missing.** Masking is exactly as it was before the rule existed.
  Doubt keeps the mask on.

## Rules

- **CW-1** The list MUST only stop the text-evidence rule in
  `code/backend/PapinianGuard/PapinianGuardService/PapinianGuardService.cpp`
  from treating a detected name as an ordinary word. It MUST NOT unmask a
  detected name, and it MUST NOT mask a word.
- **CW-2** If the file is missing, `is_common_word()`
  (`code/backend/PapinianGuard/CommonWords/CommonWords.hpp`) answers false for
  everything, and the text-evidence rule never fires. Papinian Guard then masks
  exactly as it did before the rule existed.
- **CW-3** The file stays a plain list: one lowercase word per line, sorted,
  ASCII only, three to sixteen letters.

## Context

- **Provenance.** `english_common_words.txt` holds every lowercase entry of three
  to sixteen letters from *Webster's Second International Dictionary* (1934).
  That dictionary is in the public domain, and it is distributed with the BSDs
  and macOS as `/usr/share/dict/web2`.
- **Why it acts in one direction only.** A client whose surname is written in
  lowercase in a document is still protected: "whitfield" is not a word of the
  language. Meanwhile "children", "father" or "the buyer" can be recognised as
  ordinary words when the document itself writes them as ordinary words. The
  list can only keep an ordinary word from being treated as a name. A name is
  never sent unmasked because a list says so.
