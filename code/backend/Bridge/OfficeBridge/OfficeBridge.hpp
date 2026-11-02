#pragma once

#include <nlohmann/json.hpp>

// The Native API of Singularity in Word, Excel and Outlook
// (docs/contracts/native/Office.json, D-105): the connection's state, its
// connecting and disconnecting from Singularity's window, and what a pane
// asks of a file - its Matter, filing it into one, a document's bytes.
// Parses, calls Office/, serialises; no rule lives here.
namespace singularity::bridge::office_bridge {

nlohmann::json get_status(const nlohmann::json &params);
nlohmann::json connect(const nlohmann::json &params);
nlohmann::json disconnect(const nlohmann::json &params);
// office.openApp: Word, Excel, PowerPoint or Outlook opened as the person
// connects it, from Singularity's window only (D-191): no pane reaches it.
nlohmann::json open_app(const nlohmann::json &params);
// The panes waiting for the person's answer and the panes paired here; the
// answer; a pane's removal. Singularity's window only: no pane reaches them.
nlohmann::json list_clients(const nlohmann::json &params);
nlohmann::json decide_pairing(const nlohmann::json &params);
nlohmann::json remove_client(const nlohmann::json &params);
nlohmann::json describe_file(const nlohmann::json &params);
nlohmann::json file_into_matter(const nlohmann::json &params);
nlohmann::json document_content(const nlohmann::json &params);
// office.emailDraft: a colleague's email draft of a Matter - its recipients,
// subject, words and attachments - for Outlook's pane to put into a letter
// being written at the person's press (D-119). It never sends.
nlohmann::json email_draft(const nlohmann::json &params);
nlohmann::json open_document(const nlohmann::json &params);
// A place in a document to show in Word or Excel - from Singularity's window
// only: no pane reaches it.
nlohmann::json reveal(const nlohmann::json &params);

} // namespace singularity::bridge::office_bridge
