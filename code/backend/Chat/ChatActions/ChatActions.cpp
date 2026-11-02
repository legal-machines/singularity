#include "Chat/ChatActions/ChatActions.hpp"
#include "Chat/MessageProvenance/MessageProvenance.hpp"

#include "Document/ArtifactCapabilityRegistry/ArtifactCapabilityRegistry.hpp"
#include "ExternalResearch/ContextBudget/ContextBudget.hpp"
#include "Chat/ChatContinuity/ChatContinuity.hpp"

#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/TextFold/TextFold.hpp"
#include "Routing/AiSetup/AiSetup.hpp"

#include <nlohmann/json.hpp>
#include <utf8proc.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <filesystem>
#include <mutex>
#include <fstream>
#include <set>
#include <stdexcept>

namespace singularity::chat {
namespace {

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

auto find_project(std::vector<matters::Matter> &projects, const std::string &project_id) {
  return std::find_if(projects.begin(), projects.end(),
      [&](const matters::Matter &entry) { return entry.id == project_id; });
}

auto find_chat(matters::Matter &project, const std::string &chat_id) {
  return std::find_if(project.chats.begin(), project.chats.end(),
      [&](const matters::MatterChat &entry) { return entry.id == chat_id; });
}

// Text on one line, its runs of spaces as one: how a place's words are kept.
std::string one_line(const std::string &value) {
  std::string line;
  bool space = false;
  for (const char character : value) {
    const bool isSpace = character == ' ' || character == '\t' || character == '\r' || character == '\n';
    if (isSpace) {
      space = !line.empty();
      continue;
    }
    if (space) line.push_back(' ');
    space = false;
    line.push_back(character);
  }
  return line;
}

// What a message carries, checked against the Matter it is written in: the
// message it replies to is of the same thread, every document it hands over is
// one of the Matter's, the line of what was not checked goes with files, the
// answers it offers are two to four distinct ones, and an answer it gives is
// one the message it quotes offers.
MessageExtras checked_extras(const matters::Matter &project, const std::string &chat_id, const MessageExtras &extras) {
  MessageExtras checked;
  const matters::MatterTask *quoted = nullptr;
  if (!extras.quote_message_id.empty()) {
    const auto found = std::find_if(project.tasks.begin(), project.tasks.end(), [&](const matters::MatterTask &task) {
      return task.id == extras.quote_message_id && task.chat_id == std::optional<std::string>(chat_id);
    });
    if (found == project.tasks.end()) throw std::invalid_argument("The message this replies to is not in this thread.");
    quoted = &*found;
    checked.quote_message_id = extras.quote_message_id;
  }
  if (extras.attachment_document_ids.size() > 10) throw std::invalid_argument("A message can hand over at most 10 files.");
  for (const std::string &document_id : extras.attachment_document_ids) {
    const bool isDocument = std::any_of(project.documents.begin(), project.documents.end(),
        [&](const matters::MatterDocument &document) { return document.id == document_id; });
    if (!isDocument) throw std::invalid_argument("A file this message hands over is not a document of this Matter.");
    if (std::find(checked.attachment_document_ids.begin(), checked.attachment_document_ids.end(), document_id)
        == checked.attachment_document_ids.end())
      checked.attachment_document_ids.push_back(document_id);
  }
  const std::string not_checked = trim(extras.not_checked);
  if (!not_checked.empty()) {
    if (checked.attachment_document_ids.empty())
      throw std::invalid_argument("What was not checked goes with the files a message hands over.");
    if (not_checked.size() > 1000) throw std::invalid_argument("What was not checked is longer than 1,000 characters.");
    checked.not_checked = not_checked;
  }
  if (!extras.ask_options.empty()) {
    if (extras.ask_options.size() < 2 || extras.ask_options.size() > 4)
      throw std::invalid_argument("A question offers two to four answers.");
    for (const std::string &option : extras.ask_options) {
      const std::string words = trim(option);
      if (words.empty()) throw std::invalid_argument("Each answer a question offers needs its words.");
      if (words.size() > 120) throw std::invalid_argument("An answer a question offers is longer than 120 characters.");
      if (std::find(checked.ask_options.begin(), checked.ask_options.end(), words) != checked.ask_options.end())
        throw std::invalid_argument("A question offers each answer once.");
      checked.ask_options.push_back(words);
    }
  }
  const std::string answer = trim(extras.answer_option);
  if (!answer.empty()) {
    if (quoted == nullptr || std::find(quoted->ask_options.begin(), quoted->ask_options.end(), answer) == quoted->ask_options.end())
      throw std::invalid_argument("This answer is not one the question offers.");
    if (!checked.ask_options.empty()) throw std::invalid_argument("An answer does not ask a question of its own.");
    checked.answer_option = answer;
  }
  // The place it is about is in one of the Matter's documents, named by its
  // section or by the words there.
  if (!extras.place.empty()) {
    const bool isDocument = std::any_of(project.documents.begin(), project.documents.end(),
        [&](const matters::MatterDocument &document) { return document.id == extras.place.document_id; });
    if (!isDocument) throw std::invalid_argument("The place this message is about is not in a document of this Matter.");
    matters::DocumentPlace place;
    place.document_id = extras.place.document_id;
    place.section = one_line(extras.place.section);
    place.heading = one_line(extras.place.heading);
    place.quote = one_line(extras.place.quote);
    if (place.section.size() > 40) throw std::invalid_argument("A section's number is longer than 40 characters.");
    if (place.heading.size() > 200) throw std::invalid_argument("A section's heading is longer than 200 characters.");
    if (place.quote.size() > 1200) throw std::invalid_argument("The words quoted from the document are longer than 1,200 characters.");
    if (place.section.empty() && place.quote.empty())
      throw std::invalid_argument("A place in a document names its section or quotes its words.");
    checked.place = place;
  }
  // Whom it names, as a person's message names them: safe ids, each once.
  if (extras.mentions.size() > 20) throw std::invalid_argument("A message can name at most 20 colleagues and agents.");
  for (const std::string &mention : extras.mentions) {
    const bool isSafe = !mention.empty() && mention.size() <= 120
        && std::all_of(mention.begin(), mention.end(), [](char character) {
             return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' || character == ':' || character == '-';
           });
    if (!isSafe) throw std::invalid_argument("A message names someone Singularity does not know.");
    if (std::find(checked.mentions.begin(), checked.mentions.end(), mention) == checked.mentions.end())
      checked.mentions.push_back(mention);
  }
  return checked;
}

// Whether a thread stands closed: its latest closing has no reopening, and no
// message, after it.
bool is_thread_closed(const matters::Matter &project, const std::string &chat_id) {
  bool closed = false;
  for (const matters::MatterTask &task : project.tasks) {
    if (task.chat_id != std::optional<std::string>(chat_id)) continue;
    if (task.thread_event == "closed") closed = true;
    else if (task.thread_event == "reopened") closed = false;
    else if (task.operation_type == "member_message" || task.operation_type == "agent_message") closed = false;
  }
  return closed;
}

// A thread's closing or reopening, as a record of the member at this computer,
// signed as its messages are. Not saved: the caller saves the Matter.
matters::MatterTask thread_record(const std::string &project_id, const std::string &chat_id, const std::string &event,
                                  const std::string &words, const platform::ActorIdentity &identity,
                                  const std::string &at = std::string()) {
  const std::string now = at.empty() ? workspaces::current_timestamp() : at;
  matters::MatterTask record;
  record.id = matters::generate_task_id();
  record.created_at = now;
  record.operation_type = "member_message";
  record.operation_execution_state = "completed";
  record.operation_started_at = now;
  record.operation_updated_at = now;
  record.chat_id = chat_id;
  record.prompt = words;
  record.author_actor_id = identity.actor_id;
  record.author_name = identity.display_name.value_or(std::string());
  record.thread_event = event;
  record.status = matters::TaskStatus::Success;
  sign_member_message(project_id, record);
  return record;
}

// The person's message `message_id` of the thread, for its author to change on
// the computer that wrote it - the same rule for every member.
matters::MatterTask &own_message(matters::Matter &project, const std::string &chat_id, const std::string &message_id) {
  const auto found = std::find_if(project.tasks.begin(), project.tasks.end(), [&](const matters::MatterTask &task) {
    return task.id == message_id && task.chat_id == std::optional<std::string>(chat_id);
  });
  if (found == project.tasks.end()) throw std::runtime_error("This message is no longer in the thread.");
  if (found->operation_type != "member_message" || !found->deleted_at.empty())
    throw std::runtime_error("Only its author can change this message.");
  if (!found->thread_event.empty())
    throw std::runtime_error("A thread's closing or reopening stays in it as it was made.");
  const platform::ActorIdentity identity = platform::resolve_actor_identity();
  if (found->author_actor_id != identity.actor_id
      || member_message_provenance(project.id, *found) != MessageProvenance::ThisComputer)
    throw std::runtime_error("Only its author can change a message, on the computer it was written on.");
  return *found;
}

// Real Unicode case-folding via utf8proc (never a hand-rolled case table) -
// Phase 27's Search is a real backend text match, not a frontend filter, and
// must not silently miss a match that only differs by case/composition.
std::string casefold(const std::string &value) {
  utf8proc_uint8_t *mapped = nullptr;
  const utf8proc_ssize_t mapped_length = utf8proc_map(
      reinterpret_cast<const utf8proc_uint8_t *>(value.data()), static_cast<utf8proc_ssize_t>(value.size()),
      &mapped, static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD));
  if (mapped_length < 0 || mapped == nullptr) {
    if (mapped != nullptr) std::free(mapped);
    return value;
  }
  std::string result(reinterpret_cast<const char *>(mapped), static_cast<std::size_t>(mapped_length));
  std::free(mapped);
  return result;
}

// The nearest UTF-8 character boundary at or before `position` - defends
// snippet slicing against landing mid-sequence, since a case-folded match
// position can drift a few bytes from the original text on scripts where
// folding changes byte length (e.g. German "ß").
std::size_t previous_utf8_boundary(const std::string &text, std::size_t position) {
  position = std::min(position, text.size());
  while (position > 0 && (static_cast<unsigned char>(text[position]) & 0xC0) == 0x80) --position;
  return position;
}

std::string build_snippet(const std::string &text, std::size_t match_position, std::size_t match_length) {
  constexpr std::size_t context = 40;
  const std::size_t start = previous_utf8_boundary(text, match_position > context ? match_position - context : 0);
  const std::size_t end = previous_utf8_boundary(text, std::min(text.size(), match_position + match_length + context));
  std::string snippet = text.substr(start, end - start);
  if (start > 0) snippet = "…" + snippet;
  if (end < text.size()) snippet += "…";
  return snippet;
}

} // namespace

matters::ChatList list(const std::string &project_id) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found = matters::load_matter(workspace_path, project_id);
  if (!found) throw std::runtime_error("This Project is no longer available.");

  matters::ChatList result;
  result.project_id = found->id;
  result.project_name = found->name;
  result.project_instructions_active = found->instructions.has_value() && trim(*found->instructions) != "";
  // A deleted Chat stays out of the list - unless work reached it after the
  // deletion (another device's turn), in which case it is listed again so the
  // person can see and restore it.
  for (const matters::MatterChat &chat : found->chats) {
    if (chat.deleted_at.empty()) {
      result.chats.push_back(chat);
      continue;
    }
    const bool workAfterDeletion = std::any_of(found->tasks.begin(), found->tasks.end(),
        [&](const matters::MatterTask &task) {
          return task.chat_id && *task.chat_id == chat.id && task.created_at > chat.deleted_at;
        });
    if (workAfterDeletion) result.chats.push_back(chat);
  }
  std::sort(result.chats.begin(), result.chats.end(), [](const auto &left, const auto &right) {
    if (left.updated_at != right.updated_at) return left.updated_at > right.updated_at;
    if (left.created_at != right.created_at) return left.created_at > right.created_at;
    return left.id < right.id;
  });

  for (const matters::MatterTask &task : found->tasks) {
    if (!task.chat_id) continue;
    const auto chat = std::find_if(found->chats.begin(), found->chats.end(),
        [&](const matters::MatterChat &entry) { return entry.id == *task.chat_id; });
    if (chat != found->chats.end()) result.task_links.push_back({task.id, *task.chat_id});
  }
  std::sort(result.task_links.begin(), result.task_links.end(), [](const auto &left, const auto &right) {
    if (left.chat_id != right.chat_id) return left.chat_id < right.chat_id;
    return left.task_id < right.task_id;
  });
  return result;
}

matters::Matter create(const std::string& matter_id, std::string title,
                       std::vector<std::string> selected_document_ids) {
  title = trim(std::move(title));
  if (title.empty()) throw std::runtime_error("Enter a chat title.");
  const platform::ActorIdentity actor = platform::resolve_actor_identity();
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found = matters::load_matter(workspace_path, matter_id);
  if (!found) throw std::runtime_error("This matter no longer exists.");
  matters::MatterChat created;
  created.id = matters::generate_chat_id();
  created.project_id = found->id;
  created.title = std::move(title);
  created.title_authority = created.title == "New chat"
      ? matters::ChatTitleAuthority::Default : matters::ChatTitleAuthority::Automatic;
  created.created_at = workspaces::current_timestamp();
  created.updated_at = created.created_at;
  created.created_by_actor_id = actor.actor_id;
  created.updated_by_actor_id = actor.actor_id;
  created.operation_id = matters::generate_chat_operation_id();
  created.selected_document_ids = std::move(selected_document_ids);
  found->chats.push_back(created);
  found->updated_at = created.updated_at;
  matters::save_matter(workspace_path, *found);
  return *found;
}

matters::MatterChat create(const std::string &project_id) {
  const matters::Matter project = create(project_id, "New chat");
  if (project.chats.empty()) throw std::runtime_error("Singularity could not create this Chat.");
  return project.chats.back();
}

std::vector<std::string> conversation_chats(const std::string &project_id) {
  std::vector<matters::MatterChat> standing;
  {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    const auto workspace_path = node::require_active_workspace_path();
    const auto found = matters::load_matter(workspace_path, project_id);
    if (!found) throw std::runtime_error("This conversation is not on this computer.");
    for (const matters::MatterChat &chat : found->chats) {
      if (chat.deleted_at.empty() && !chat.archived) standing.push_back(chat);
    }
  }
  // Both computers may begin it at once: the earliest stands for both, and one
  // begun in the same second is ordered by its id, the same on both.
  std::sort(standing.begin(), standing.end(), [](const matters::MatterChat &one, const matters::MatterChat &other) {
    return one.created_at != other.created_at ? one.created_at < other.created_at : one.id < other.id;
  });
  std::vector<std::string> ids;
  for (const matters::MatterChat &chat : standing) ids.push_back(chat.id);
  return ids;
}

std::string conversation_chat(const std::string &project_id, bool make) {
  const std::vector<std::string> standing = conversation_chats(project_id);
  if (!standing.empty() || !make) return standing.empty() ? std::string() : standing.front();
  return create(project_id).id;
}

namespace {

// Byte length of the UTF-8 sequence starting with `lead`.
std::size_t utf8_sequence_length(unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead >> 5) == 0x6) return 2;
  if ((lead >> 4) == 0xE) return 3;
  if ((lead >> 3) == 0x1E) return 4;
  return 1;
}

// Byte offsets of every character start, plus the end offset.
std::vector<std::size_t> character_offsets(const std::string &text) {
  std::vector<std::size_t> offsets;
  std::size_t index = 0;
  while (index < text.size()) {
    offsets.push_back(index);
    index += utf8_sequence_length(static_cast<unsigned char>(text[index]));
  }
  offsets.push_back(std::min(index, text.size()));
  return offsets;
}

bool is_title_space(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

const std::string kEllipsis = "\xE2\x80\xA6";

// A title never ends on a word that only joins: one cut at a model's or a
// line's limit ("Brightwater's IC meets on November 10 and", 29 Oct 2026)
// ends on the last word that says something. Two words are always kept.
std::string without_dangling_word(std::string title) {
  static const std::set<std::string> joining{"a", "an", "and", "as", "at", "but", "by", "for", "from", "in", "into",
    "its", "nor", "of", "on", "or", "our", "than", "that", "the", "their", "to", "which", "with", "your"};
  while (true) {
    title = trim(std::move(title));
    while (!title.empty() && (title.back() == ',' || title.back() == ';' || title.back() == ':' || title.back() == '-'))
      title = trim(title.substr(0, title.size() - 1));
    const std::size_t space = title.rfind(' ');
    if (space == std::string::npos || title.find(' ') == space) return title;
    std::string last = title.substr(space + 1);
    std::transform(last.begin(), last.end(), last.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (joining.count(last) == 0) return title;
    title = title.substr(0, space);
  }
}

} // namespace

std::string derive_automatic_chat_title(const std::string &prompt) {
  constexpr std::size_t kMaxCharacters = 64;
  // The first non-empty line, with Markdown heading/list markers and
  // surrounding quotes removed, and runs of spaces collapsed.
  std::string line;
  std::size_t start = 0;
  while (start <= prompt.size()) {
    std::size_t end = prompt.find('\n', start);
    if (end == std::string::npos) end = prompt.size();
    std::string candidate = trim(prompt.substr(start, end - start));
    while (!candidate.empty() && (candidate.front() == '#' || candidate.front() == '>' ||
                                  candidate.front() == '-' || candidate.front() == '*' ||
                                  candidate.front() == '"' || candidate.front() == '\'')) {
      candidate = trim(candidate.substr(1));
    }
    if (!candidate.empty()) {
      line = std::move(candidate);
      break;
    }
    if (end == prompt.size()) break;
    start = end + 1;
  }
  if (line.empty()) return {};
  std::string collapsed;
  for (char value : line) {
    if (is_title_space(value)) {
      if (!collapsed.empty() && collapsed.back() != ' ') collapsed.push_back(' ');
    } else {
      collapsed.push_back(value);
    }
  }
  line = trim(std::move(collapsed));

  const std::vector<std::size_t> offsets = character_offsets(line);
  const std::size_t character_count = offsets.size() - 1;

  // The first sentence, when it ends within the budget.
  for (std::size_t index = 0; index < character_count && index < kMaxCharacters; ++index) {
    const char value = line[offsets[index]];
    if ((value == '?' || value == '.' || value == '!') &&
        (index + 1 == character_count || line[offsets[index + 1]] == ' ')) {
      std::string sentence = line.substr(0, offsets[index + 1]);
      if (value == '.') sentence.pop_back();
      return trim(std::move(sentence));
    }
  }
  if (character_count <= kMaxCharacters) return line;

  // Cut at the last word boundary inside the budget.
  std::size_t cut = offsets[kMaxCharacters];
  const std::size_t space = line.rfind(' ', cut);
  if (space != std::string::npos && space > offsets[kMaxCharacters / 2]) cut = space;
  std::string shortened = trim(line.substr(0, cut));
  while (!shortened.empty() && (shortened.back() == ',' || shortened.back() == ';' ||
                                shortened.back() == ':' || shortened.back() == '-' ||
                                shortened.back() == '.')) {
    shortened.pop_back();
  }
  return without_dangling_word(trim(std::move(shortened))) + kEllipsis;
}

bool apply_automatic_title_if_default(const std::string &project_id, const std::string &chat_id,
                                      const std::string &prompt) {
  try {
    const std::string title = derive_automatic_chat_title(prompt);
    if (title.empty()) return false;
    const platform::ActorIdentity actor = platform::resolve_actor_identity();
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    const auto workspace_path = node::require_active_workspace_path();
    auto found_project = matters::load_matter(workspace_path, project_id);
    if (!found_project) return false;
    const auto found_chat = find_chat(*found_project, chat_id);
    if (found_chat == found_project->chats.end()) return false;
    if (found_chat->title_authority != matters::ChatTitleAuthority::Default) return false;
    found_chat->title = title;
    found_chat->title_authority = matters::ChatTitleAuthority::Automatic;
    found_chat->updated_at = workspaces::current_timestamp();
    found_chat->revision += 1;
    found_chat->updated_by_actor_id = actor.actor_id;
    found_chat->operation_id = matters::generate_chat_operation_id();
    found_project->updated_at = found_chat->updated_at;
    matters::save_matter(workspace_path, *found_project);
    return true;
  } catch (...) {
    return false;
  }
}

bool apply_generated_title_if_default(const std::string &project_id, const std::string &chat_id,
                                      const std::string &title) {
  try {
    const std::string name = trim(title);
    if (name.empty()) return false;
    const platform::ActorIdentity actor = platform::resolve_actor_identity();
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    const auto workspace_path = node::require_active_workspace_path();
    auto found_project = matters::load_matter(workspace_path, project_id);
    if (!found_project) return false;
    const auto found_chat = find_chat(*found_project, chat_id);
    if (found_chat == found_project->chats.end()) return false;
    if (found_chat->title_authority != matters::ChatTitleAuthority::Default) return false;
    found_chat->title = name;
    found_chat->title_authority = matters::ChatTitleAuthority::Automatic;
    found_chat->updated_at = workspaces::current_timestamp();
    found_chat->revision += 1;
    found_chat->updated_by_actor_id = actor.actor_id;
    found_chat->operation_id = matters::generate_chat_operation_id();
    found_project->updated_at = found_chat->updated_at;
    matters::save_matter(workspace_path, *found_project);
    return true;
  } catch (...) {
    return false;
  }
}

namespace {

// A word of a name or of a message: a run of letters, marks and digits in any
// script (utf8proc's categories, never a table of ours), and where it stands.
struct NameWord {
  std::size_t start = 0;
  std::size_t end = 0;
  std::string text;
};

std::vector<utf8proc_int32_t> codepoints_of(const std::string &text) {
  std::vector<utf8proc_int32_t> codepoints;
  std::size_t index = 0;
  while (index < text.size()) {
    utf8proc_int32_t codepoint = -1;
    const utf8proc_ssize_t read = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data() + index),
        static_cast<utf8proc_ssize_t>(text.size() - index), &codepoint);
    if (read <= 0) return {};
    codepoints.push_back(codepoint);
    index += static_cast<std::size_t>(read);
  }
  return codepoints;
}

std::vector<NameWord> name_words(const std::string &text) {
  std::vector<NameWord> words;
  std::size_t start = std::string::npos;
  std::size_t index = 0;
  while (index <= text.size()) {
    bool in_word = false;
    std::size_t step = 1;
    if (index < text.size()) {
      utf8proc_int32_t codepoint = -1;
      const utf8proc_ssize_t read = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data() + index),
          static_cast<utf8proc_ssize_t>(text.size() - index), &codepoint);
      if (read > 0) {
        step = static_cast<std::size_t>(read);
        const utf8proc_category_t category = utf8proc_category(codepoint);
        in_word = category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_NO;
      }
    }
    if (in_word && start == std::string::npos) start = index;
    if (!in_word && start != std::string::npos) {
      words.push_back({start, index, text.substr(start, index - start)});
      start = std::string::npos;
    }
    if (index == text.size()) break;
    index += step;
  }
  return words;
}

// How a word is written: opening with a capital, with a capital inside it
// (SPA, McKinsey), or in lower case throughout. A figure is none of these.
struct WordCase {
  bool opens_capital = false;
  bool capital_inside = false;
  bool lower = false;
};

WordCase case_of(const std::string &word) {
  WordCase shape;
  bool letters = false;
  const std::vector<utf8proc_int32_t> codepoints = codepoints_of(word);
  for (std::size_t index = 0; index < codepoints.size(); ++index) {
    const utf8proc_category_t category = utf8proc_category(codepoints[index]);
    if (category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_LO) letters = true;
    if (category != UTF8PROC_CATEGORY_LU && category != UTF8PROC_CATEGORY_LT) continue;
    if (index == 0) shape.opens_capital = true;
    else shape.capital_inside = true;
  }
  shape.lower = letters && !shape.opens_capital && !shape.capital_inside;
  return shape;
}

std::string lowered(const std::string &word) {
  std::string result;
  for (const utf8proc_int32_t codepoint : codepoints_of(word)) {
    utf8proc_uint8_t encoded[4] = {0, 0, 0, 0};
    const utf8proc_ssize_t written = utf8proc_encode_char(utf8proc_tolower(codepoint), encoded);
    if (written <= 0) return word;
    result.append(reinterpret_cast<const char *>(encoded), static_cast<std::size_t>(written));
  }
  return result.empty() ? word : result;
}

// The words a sentence writes in lower case wherever they stand, and that
// Title Case gives a capital: never a name.
bool is_small_word(const std::string &folded) {
  static const std::set<std::string> small{"a", "about", "after", "against", "an", "and", "as", "at", "before",
    "between", "but", "by", "concerning", "for", "from", "in", "into", "nor", "of", "on", "or", "over", "per", "re",
    "regarding", "the", "to", "under", "upon", "via", "vs", "with", "without", "в", "во", "и", "к", "до", "для",
    "за", "из", "на", "о", "об", "от", "по", "при", "с", "со"};
  return small.count(folded) > 0;
}

// "Conversation about", "Question on", "Discussion of": what the chat is,
// said before what it is about.
bool names_the_chat(const std::string &first, const std::string &second) {
  static const std::set<std::string> kinds{"chat", "conversation", "dialogue", "discussion", "enquiry", "inquiry",
    "query", "question", "questions", "talk"};
  static const std::set<std::string> joins{"about", "concerning", "on", "re", "regarding"};
  return kinds.count(first) > 0 && (joins.count(second) > 0 || (first == "discussion" && second == "of"));
}

} // namespace

std::string chat_title_from_reply(const std::string &reply, const std::string &first_message) {
  std::string text = reply;
  // A reasoning model may think aloud first: what it concluded comes after.
  const auto thought = text.rfind("</think>");
  if (thought != std::string::npos) text = text.substr(thought + 8);
  std::string line;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string candidate = trim(text.substr(start, end - start));
    if (!candidate.empty()) line = std::move(candidate);
    if (end == text.size()) break;
    start = end + 1;
  }
  for (const char *prefix : {"Title:", "title:", "Name:", "name:"}) {
    if (line.rfind(prefix, 0) == 0) line = line.substr(std::string(prefix).size());
  }
  {
    const char *marks = " \t\r\"'`*#";
    const auto first = line.find_first_not_of(marks);
    const auto last = line.find_last_not_of(std::string(marks) + ".:,;");
    line = first == std::string::npos || last == std::string::npos || last < first
        ? std::string() : line.substr(first, last - first + 1);
  }

  // The model's capitals are read before anything is taken away: Title Case
  // shows in a small word written with a capital ("About"), or in every later
  // word opening with one.
  const std::vector<NameWord> written = name_words(line);
  bool title_case = false;
  {
    std::size_t later = 0;
    std::size_t capitalised = 0;
    bool small_capital = false;
    for (std::size_t index = 1; index < written.size(); ++index) {
      const WordCase shape = case_of(written[index].text);
      if (is_small_word(casefold(written[index].text))) {
        small_capital = small_capital || shape.opens_capital;
        continue;
      }
      if (shape.capital_inside || (!shape.opens_capital && !shape.lower)) continue;
      ++later;
      if (shape.opens_capital) ++capitalised;
    }
    title_case = later > 0 && capitalised == later && (later >= 2 || small_capital);
  }
  if (written.size() >= 3 && names_the_chat(casefold(written[0].text), casefold(written[1].text)))
    line = trim(line.substr(written[2].start));

  // How the lawyer wrote the words of their message.
  std::map<std::string, std::string> as_written;
  std::map<std::string, std::string> in_lower;
  std::set<std::string> with_capital;
  for (const NameWord &word : name_words(first_message)) {
    const WordCase shape = case_of(word.text);
    const std::string folded = casefold(word.text);
    if (shape.capital_inside) as_written.emplace(folded, word.text);
    else if (shape.lower) in_lower.emplace(folded, word.text);
    else if (shape.opens_capital) with_capital.insert(folded);
  }

  const std::vector<NameWord> words = name_words(line);
  std::vector<std::string> kept;
  for (const NameWord &word : words) {
    const auto lawyers = as_written.find(casefold(word.text));
    kept.push_back(lawyers != as_written.end() ? lawyers->second : word.text);
  }
  if (title_case) {
    std::vector<std::string> sentence = kept;
    bool accounted = true;
    for (std::size_t index = 1; index < kept.size() && accounted; ++index) {
      const WordCase shape = case_of(kept[index]);
      if (!shape.opens_capital || shape.capital_inside) continue;
      const std::string folded = casefold(kept[index]);
      const auto lower = in_lower.find(folded);
      if (is_small_word(folded)) sentence[index] = lowered(kept[index]);
      else if (lower != in_lower.end()) sentence[index] = lower->second;
      else if (with_capital.count(folded) == 0) accounted = false;
    }
    if (accounted) kept = std::move(sentence);
  }
  std::string named;
  std::size_t at = 0;
  for (std::size_t index = 0; index < words.size(); ++index) {
    named += line.substr(at, words[index].start - at);
    named += kept[index];
    at = words[index].end;
  }
  named = without_dangling_word(trim(named + line.substr(std::min(at, line.size()))));

  // A name is a few words. Anything else is not a name, and is not kept.
  const auto spaces = static_cast<std::size_t>(std::count(named.begin(), named.end(), ' '));
  const std::size_t characters = character_offsets(named).size() - 1;
  if (characters < 3 || characters > 80 || spaces + 1 > 9) return {};
  // And a name opens with a capital, in whatever language it was written.
  return platform::with_initial_capital(named);
}

matters::MatterChat rename(const std::string &project_id, const std::string &chat_id,
                           std::string title, long long expected_revision,
                           matters::ChatTitleAuthority authority) {
  title = trim(std::move(title));
  if (title.empty()) throw std::runtime_error("Enter a chat title.");
  if (expected_revision < 1) throw std::runtime_error("This Chat revision is invalid.");
  const platform::ActorIdentity actor = platform::resolve_actor_identity();
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");
  const auto found_chat = find_chat(*found_project, chat_id);
  if (found_chat == found_project->chats.end()) throw std::runtime_error("This Chat is no longer available.");
  if (found_chat->revision != expected_revision) {
    throw std::runtime_error("This Chat changed since you opened it. Review its current title and try again.");
  }
  if (authority == matters::ChatTitleAuthority::Automatic &&
      found_chat->title_authority == matters::ChatTitleAuthority::User) {
    throw std::runtime_error("This Chat title was chosen by a person and cannot be changed automatically.");
  }

  found_chat->title = std::move(title);
  found_chat->title_authority = authority;
  found_chat->updated_at = workspaces::current_timestamp();
  found_chat->revision += 1;
  found_chat->updated_by_actor_id = actor.actor_id;
  found_chat->operation_id = matters::generate_chat_operation_id();
  found_project->updated_at = found_chat->updated_at;
  matters::save_matter(workspace_path, *found_project);
  return *found_chat;
}

matters::MatterChat set_archived(const std::string &project_id, const std::string &chat_id, bool archived) {
  const platform::ActorIdentity actor = platform::resolve_actor_identity();
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Matter is no longer available.");
  const auto found_chat = find_chat(*found_project, chat_id);
  if (found_chat == found_project->chats.end() || !found_chat->deleted_at.empty())
    throw std::runtime_error("This thread is no longer here.");
  if (found_chat->archived == archived) return *found_chat;
  found_chat->archived = archived;
  found_chat->updated_at = workspaces::current_timestamp();
  found_chat->revision += 1;
  found_chat->updated_by_actor_id = actor.actor_id;
  found_project->updated_at = found_chat->updated_at;
  matters::save_matter(workspace_path, *found_project);
  return *found_chat;
}

matters::Matter save_ai_setup_overrides(const std::string &matter_id, const std::string &chat_id,
                                        std::vector<matters::AiSetupRoleOverride> overrides) {
  // Structural validation only - see MatterActions.cpp's
  // save_ai_setup_overrides() for why the live-registry compatibility check
  // stays in the Bridge layer (Bridge/ChatBridge/ChatBridge.cpp).
  for (const auto &override_entry : overrides) {
    if (!routing::ai_setup_role_from_string(override_entry.role)) {
      throw std::runtime_error("This is not a real AI Setup role.");
    }
  }

  const platform::ActorIdentity actor = platform::resolve_actor_identity();
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_matter = matters::load_matter(workspace_path, matter_id);
  if (!found_matter) throw std::runtime_error("This matter no longer exists.");
  const auto found_chat = std::find_if(found_matter->chats.begin(), found_matter->chats.end(),
      [&](const matters::MatterChat &entry) { return entry.id == chat_id; });
  if (found_chat == found_matter->chats.end()) throw std::runtime_error("This chat no longer exists.");
  found_chat->ai_setup_overrides = std::move(overrides);
  found_chat->updated_at = workspaces::current_timestamp();
  found_chat->revision += 1;
  found_chat->updated_by_actor_id = actor.actor_id;
  found_chat->operation_id = matters::generate_chat_operation_id();
  matters::save_matter(workspace_path, *found_matter);
  return *found_matter;
}

matters::MatterChat save_draft(const std::string &project_id, const std::string &chat_id, std::string draft_text) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");
  const auto found_chat = find_chat(*found_project, chat_id);
  if (found_chat == found_project->chats.end()) throw std::runtime_error("This Chat is no longer available.");
  // Deliberately does not bump revision/updatedAt/updatedByActorId: an
  // ordinary debounced draft keystroke is not the structural change
  // rename()/save_ai_setup_overrides() protect with optimistic locking, and
  // bumping updatedAt would reorder the Chat list every time a person merely
  // typed a draft in a Chat they never actually sent from.
  found_chat->draft_text = std::move(draft_text);
  matters::save_matter(workspace_path, *found_project);
  return *found_chat;
}

matters::MatterChat set_working_document(const std::string &project_id, const std::string &chat_id,
                                        const std::string &document_id) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");
  const auto found_chat = find_chat(*found_project, chat_id);
  if (found_chat == found_project->chats.end()) throw std::runtime_error("This Chat is no longer available.");
  if (document_id == matters::kNewDocumentWorkingTarget) {
    if (!found_project->directory_path)
      throw std::runtime_error("This Project has no folder a new document can be written into.");
  } else if (!document_id.empty()) {
    const auto found_document = std::find_if(found_project->documents.begin(), found_project->documents.end(),
        [&](const matters::MatterDocument &entry) { return entry.id == document_id; });
    if (found_document == found_project->documents.end())
      throw std::runtime_error("That document is not in this Project.");
    if (found_document->source_mode != matters::DocumentSourceMode::Referenced)
      throw std::runtime_error("Only a document in the Project folder can be worked on.");
    if (!documents::artifact_format_capability(found_document->file_type).can_update())
      throw std::runtime_error("Singularity cannot write this kind of file yet, so it cannot be worked on.");
  }
  found_chat->working_document_id = document_id;
  matters::save_matter(workspace_path, *found_project);
  return *found_chat;
}

matters::MatterChat bind_agent_thread(const std::string &project_id, const std::string &chat_id,
                                     const std::string &agent_id, const std::string &instructions) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");
  const auto found_chat = find_chat(*found_project, chat_id);
  if (found_chat == found_project->chats.end()) throw std::runtime_error("This Chat is no longer available.");
  if (found_chat->agent_id == agent_id && found_chat->instructions == instructions) return *found_chat;
  found_chat->agent_id = agent_id;
  found_chat->instructions = instructions;
  matters::save_matter(workspace_path, *found_project);
  return *found_chat;
}

std::string post_agent_message(const std::string &project_id, const std::string &chat_id,
                               const std::string &agent_id, const std::string &agent_name, const std::string &text,
                               bool kept_here, const MessageExtras &extras) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");
  const auto found_chat = find_chat(*found_project, chat_id);
  if (found_chat == found_project->chats.end()) throw std::runtime_error("This Chat is no longer available.");
  const std::string now = workspaces::current_timestamp();
  matters::MatterTask message;
  message.id = matters::generate_task_id();
  message.created_at = now;
  message.operation_type = "agent_message";
  message.operation_execution_state = "completed";
  message.operation_started_at = now;
  message.operation_updated_at = now;
  message.chat_id = chat_id;
  message.response_text = text;
  message.agent_id = agent_id;
  message.agent_initiated = true;
  message.author_name = agent_name;
  message.local_only = kept_here;
  const MessageExtras carried = checked_extras(*found_project, chat_id, extras);
  message.quote_message_id = carried.quote_message_id;
  message.attachment_document_ids = carried.attachment_document_ids;
  message.not_checked = carried.not_checked;
  message.ask_options = carried.ask_options;
  message.answer_option = carried.answer_option;
  message.place = carried.place;
  message.mentions = carried.mentions;
  message.status = matters::TaskStatus::Success;
  // A colleague who writes in a closed thread reopens it, as a person's reply
  // does, and the thread says who reopened it.
  if (is_thread_closed(*found_project, chat_id)) {
    matters::MatterTask reopening = message;
    reopening.id = matters::generate_task_id();
    reopening.response_text = std::string();
    reopening.quote_message_id.clear();
    reopening.attachment_document_ids.clear();
    reopening.not_checked.clear();
    reopening.ask_options.clear();
    reopening.answer_option.clear();
    reopening.place = matters::DocumentPlace();
    reopening.mentions.clear();
    reopening.thread_event = "reopened";
    found_project->tasks.push_back(std::move(reopening));
  }
  found_project->tasks.push_back(std::move(message));
  found_chat->updated_at = now;
  matters::save_matter(workspace_path, *found_project);
  return found_project->tasks.back().id;
}

matters::MatterTask post_member_message(const std::string &project_id, const std::string &chat_id,
                                        const std::string &text, const std::vector<std::string> &mentions,
                                        const MessageExtras &extras) {
  const std::string words = trim(text);
  if (words.empty() && extras.attachment_document_ids.empty()) throw std::invalid_argument("Write the message first.");
  if (words.size() > 20000) throw std::invalid_argument("The message is longer than 20,000 characters.");
  if (mentions.size() > 20) throw std::invalid_argument("A message can name at most 20 colleagues and agents.");
  for (const std::string &mention : mentions) {
    const bool isSafe = !mention.empty() && mention.size() <= 120
        && std::all_of(mention.begin(), mention.end(), [](char character) {
             return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' || character == ':' || character == '-';
           });
    if (!isSafe) throw std::invalid_argument("A message names someone Singularity does not know.");
  }
  const platform::ActorIdentity identity = platform::resolve_actor_identity();
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");
  const auto found_chat = find_chat(*found_project, chat_id);
  if (found_chat == found_project->chats.end() || !found_chat->deleted_at.empty())
    throw std::runtime_error("This Chat is no longer available.");
  const std::string now = workspaces::current_timestamp();
  matters::MatterTask message;
  message.id = matters::generate_task_id();
  message.created_at = now;
  message.operation_type = "member_message";
  message.operation_execution_state = "completed";
  message.operation_started_at = now;
  message.operation_updated_at = now;
  message.chat_id = chat_id;
  message.prompt = words;
  message.author_actor_id = identity.actor_id;
  message.author_name = identity.display_name.value_or(std::string());
  message.mentions = mentions;
  const MessageExtras carried = checked_extras(*found_project, chat_id, extras);
  message.quote_message_id = carried.quote_message_id;
  message.attachment_document_ids = carried.attachment_document_ids;
  message.not_checked = carried.not_checked;
  message.ask_options = carried.ask_options;
  message.answer_option = carried.answer_option;
  message.place = carried.place;
  message.status = matters::TaskStatus::Success;
  // Signed by this computer, so a colleague's Singularity can tell it is this
  // person's and unchanged (Chat/MessageProvenance).
  sign_member_message(project_id, message);
  // A reply to a closed thread reopens it, and the thread says who reopened it.
  if (is_thread_closed(*found_project, chat_id))
    found_project->tasks.push_back(thread_record(project_id, chat_id, "reopened", std::string(), identity, now));
  found_project->tasks.push_back(message);
  found_chat->updated_at = now;
  matters::save_matter(workspace_path, *found_project);
  return message;
}

bool is_workbook(const matters::MatterDocument &document) {
  return document.file_type == documents::DocumentFormat::xlsx || document.file_type == documents::DocumentFormat::csv
      || document.file_type == documents::DocumentFormat::tsv;
}

bool is_presentation(const matters::MatterDocument &document) {
  return document.file_type == documents::DocumentFormat::pptx;
}

std::string place_words(const matters::Matter &project, const matters::DocumentPlace &place) {
  if (place.empty()) return std::string();
  std::string document = "a document of the Matter";
  bool isWorkbook = false;
  bool isPresentation = false;
  for (const matters::MatterDocument &entry : project.documents) {
    if (entry.id != place.document_id) continue;
    document = "\u201C" + matters::placed_name(entry) + "\u201D";
    isWorkbook = is_workbook(entry);
    isPresentation = is_presentation(entry);
  }
  // A slide of a deck (D-112): its number, its title and what it reads.
  if (isPresentation && !place.section.empty()) {
    std::string where = place.section;
    where[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(where[0])));
    if (!place.heading.empty()) where += " (" + place.heading + ")";
    std::string words = where + " of " + document;
    if (!place.quote.empty()) words += ", which reads: \u201C" + place.quote + "\u201D";
    return words;
  }
  // Cells of a workbook (D-110): the address on its sheet, and what they hold,
  // each under its column's heading.
  if (isWorkbook && !place.section.empty()) {
    const bool isOne = place.section.find(':') == std::string::npos;
    std::string words = std::string(isOne ? "the cell " : "the cells ") + place.section;
    if (!place.heading.empty()) words += " of the " + place.heading + " sheet";
    words += " of " + document;
    if (!place.quote.empty()) words += std::string(isOne ? ", which holds: " : ", which hold: ") + "\u201C" + place.quote + "\u201D";
    return words;
  }
  std::string where = place.section.empty() ? std::string("a passage") : "Section " + place.section;
  if (!place.heading.empty()) where += " (" + place.heading + ")";
  std::string words = where + " of " + document;
  if (!place.quote.empty()) words += ", where it reads: \u201C" + place.quote + "\u201D";
  return words;
}

std::string words_for_colleagues(const matters::Matter &project, const matters::MatterTask &message) {
  if (message.place.empty()) return message.prompt;
  return message.prompt + "\n\nIt is about " + place_words(project, message.place) + ".";
}

std::string words_for_colleagues(const std::string &project_id, const matters::MatterTask &message) {
  if (message.place.empty()) return message.prompt;
  std::optional<matters::Matter> project;
  try {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    project = matters::load_matter(node::require_active_workspace_path(), project_id);
  } catch (const std::exception &) {
  }
  if (!project) return message.prompt;
  return words_for_colleagues(*project, message);
}

std::vector<std::string> addressed_colleagues(const matters::Matter &project, const matters::MatterTask &message) {
  std::vector<std::string> addressed = message.mentions;
  if (message.quote_message_id.empty()) return addressed;
  for (const matters::MatterTask &quoted : project.tasks) {
    if (quoted.id != message.quote_message_id) continue;
    const bool isColleagues = !quoted.agent_id.empty()
        && (quoted.operation_type == "agent_message" || quoted.operation_type == "matter_task");
    if (!isColleagues || !quoted.deleted_at.empty()) break;
    addressed.erase(std::remove(addressed.begin(), addressed.end(), quoted.agent_id), addressed.end());
    addressed.insert(addressed.begin(), quoted.agent_id);
    break;
  }
  return addressed;
}

std::vector<std::string> addressed_colleagues(const std::string &project_id, const matters::MatterTask &message) {
  if (message.quote_message_id.empty()) return message.mentions;
  std::optional<matters::Matter> project;
  try {
    const persistence::PersistenceLock lock(persistence::persistence_mutex());
    project = matters::load_matter(node::require_active_workspace_path(), project_id);
  } catch (const std::exception &) {
  }
  if (!project) return message.mentions;
  return addressed_colleagues(*project, message);
}

matters::MatterTask close_thread(const std::string &project_id, const std::string &chat_id,
                                 const std::string &conclusion) {
  const std::string words = trim(conclusion);
  if (words.empty()) throw std::invalid_argument("Write the thread's conclusion first.");
  if (words.size() > 20000) throw std::invalid_argument("The conclusion is longer than 20,000 characters.");
  const platform::ActorIdentity identity = platform::resolve_actor_identity();
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");
  const auto found_chat = find_chat(*found_project, chat_id);
  if (found_chat == found_project->chats.end() || !found_chat->deleted_at.empty())
    throw std::runtime_error("This Chat is no longer available.");
  if (is_thread_closed(*found_project, chat_id)) throw std::runtime_error("This thread is already closed.");
  const matters::MatterTask record = thread_record(project_id, chat_id, "closed", words, identity);
  found_project->tasks.push_back(record);
  found_chat->updated_at = record.created_at;
  matters::save_matter(workspace_path, *found_project);
  return record;
}

matters::MatterTask reopen_thread(const std::string &project_id, const std::string &chat_id) {
  const platform::ActorIdentity identity = platform::resolve_actor_identity();
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");
  const auto found_chat = find_chat(*found_project, chat_id);
  if (found_chat == found_project->chats.end() || !found_chat->deleted_at.empty())
    throw std::runtime_error("This Chat is no longer available.");
  if (!is_thread_closed(*found_project, chat_id)) throw std::runtime_error("This thread is open.");
  const matters::MatterTask record = thread_record(project_id, chat_id, "reopened", std::string(), identity);
  found_project->tasks.push_back(record);
  found_chat->updated_at = record.created_at;
  matters::save_matter(workspace_path, *found_project);
  return record;
}

matters::MatterTask edit_member_message(const std::string &project_id, const std::string &chat_id,
                                        const std::string &message_id, const std::string &text) {
  const std::string words = trim(text);
  if (words.empty()) throw std::invalid_argument("Write the message first.");
  if (words.size() > 20000) throw std::invalid_argument("The message is longer than 20,000 characters.");
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");
  matters::MatterTask &message = own_message(*found_project, chat_id, message_id);
  if (message.prompt == words) return message;
  message.prompt = words;
  message.edited_at = workspaces::current_timestamp();
  sign_member_message(project_id, message);
  const matters::MatterTask edited = message;
  matters::save_matter(workspace_path, *found_project);
  return edited;
}

matters::MatterTask delete_member_message(const std::string &project_id, const std::string &chat_id,
                                          const std::string &message_id) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");
  matters::MatterTask &message = own_message(*found_project, chat_id, message_id);
  message.prompt.clear();
  message.quote_message_id.clear();
  message.attachment_document_ids.clear();
  message.reactions.clear();
  message.place = matters::DocumentPlace();
  message.deleted_at = workspaces::current_timestamp();
  sign_member_message(project_id, message);
  const matters::MatterTask deleted = message;
  matters::save_matter(workspace_path, *found_project);
  return deleted;
}

namespace {

// One member's reaction to a message of a thread: put, or - where `toggles` -
// taken back if it was there. Returns the message's reactions as they now
// stand.
std::map<std::string, std::vector<std::string>> set_reaction(const std::string &project_id, const std::string &chat_id,
                                                             const std::string &message_id, const std::string &emoji,
                                                             const std::string &actor_id, bool toggles) {
  const bool isEmoji = !emoji.empty() && emoji.size() <= 32
      && std::none_of(emoji.begin(), emoji.end(), [](char character) {
           return static_cast<unsigned char>(character) < 0x20 || character == ' ' || character == '"' || character == '\\';
         });
  if (!isEmoji) throw std::invalid_argument("That is not a reaction.");
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");
  const auto found = std::find_if(found_project->tasks.begin(), found_project->tasks.end(), [&](const matters::MatterTask &task) {
    return task.id == message_id && task.chat_id == std::optional<std::string>(chat_id);
  });
  if (found == found_project->tasks.end()) throw std::runtime_error("This message is no longer in the thread.");
  if (!found->deleted_at.empty()) throw std::runtime_error("This message was deleted.");
  std::vector<std::string> &who = found->reactions[emoji];
  const auto mine = std::find(who.begin(), who.end(), actor_id);
  if (mine == who.end()) who.push_back(actor_id);
  else if (toggles) who.erase(mine);
  else return found->reactions;
  if (who.empty()) found->reactions.erase(emoji);
  if (found->reactions.size() > 20) throw std::invalid_argument("A message takes at most 20 different reactions.");
  const std::map<std::string, std::vector<std::string>> reactions = found->reactions;
  matters::save_matter(workspace_path, *found_project);
  return reactions;
}

} // namespace

std::map<std::string, std::vector<std::string>> react_as_colleague(const std::string &project_id, const std::string &chat_id,
                                                                   const std::string &message_id, const std::string &agent_id,
                                                                   const std::string &emoji) {
  if (agent_id.empty()) throw std::invalid_argument("A reaction needs the colleague who puts it.");
  return set_reaction(project_id, chat_id, message_id, emoji, agent_id, false);
}

std::map<std::string, std::vector<std::string>> react_to_message(const std::string &project_id,
                                                                 const std::string &chat_id,
                                                                 const std::string &message_id,
                                                                 const std::string &emoji) {
  return set_reaction(project_id, chat_id, message_id, emoji, platform::resolve_actor_identity().actor_id, true);
}

std::size_t mark_received(const std::string &project_id, const std::string &chat_id) {
  const std::string me = platform::resolve_actor_identity().actor_id;
  if (me.empty()) return 0;
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");
  const std::string now = workspaces::current_timestamp();
  std::size_t marked = 0;
  for (matters::MatterTask &task : found_project->tasks) {
    if (task.chat_id != std::optional<std::string>(chat_id) || task.operation_type != "member_message") continue;
    // Another person's words, still standing, not yet marked as here: a
    // member's own message never reaches them, and a deleted one has no words.
    if (task.author_actor_id.empty() || task.author_actor_id == me || !task.deleted_at.empty()) continue;
    if (task.received.count(me) > 0) continue;
    task.received[me] = now;
    ++marked;
  }
  if (marked > 0) matters::save_matter(workspace_path, *found_project);
  return marked;
}

ChatDeletionImpact deletion_impact(const std::string &project_id, const std::string &chat_id) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");
  const auto found_chat = std::find_if(found_project->chats.begin(), found_project->chats.end(),
      [&](const matters::MatterChat &entry) { return entry.id == chat_id; });
  if (found_chat == found_project->chats.end()) throw std::runtime_error("This Chat is no longer available.");

  std::set<std::string> chat_task_ids;
  for (const matters::MatterTask &task : found_project->tasks) {
    if (task.chat_id && *task.chat_id == chat_id && task.operation_type == "matter_task") {
      chat_task_ids.insert(task.id);
    }
  }

  ChatDeletionImpact impact;
  impact.turn_count = static_cast<int>(chat_task_ids.size());

  for (const matters::WorkProduct &product : found_project->work_products) {
    if (product.origin.kind == matters::WorkProductOriginKind::ChatTask && product.origin.reference_id
        && chat_task_ids.count(*product.origin.reference_id) > 0) {
      impact.work_product_count += 1;
      continue;
    }
    if (product.origin.kind == matters::WorkProductOriginKind::Finding && product.origin.reference_id) {
      // A Finding-origin Work Product traces back to whichever task recorded
      // that Finding id - the same lookup WorkProductActions::provenanceFor()
      // performs, never a second Finding index.
      for (const matters::MatterTask &task : found_project->tasks) {
        if (chat_task_ids.count(task.id) == 0) continue;
        const auto finding = std::find_if(task.findings.begin(), task.findings.end(),
            [&](const matters::Finding &value) { return value.id == *product.origin.reference_id; });
        if (finding != task.findings.end()) {
          impact.work_product_count += 1;
          break;
        }
      }
    }
  }

  for (const context::ContextItem &item : found_project->context_items) {
    if (item.source && chat_task_ids.count(item.source->chat_operation_id) > 0) {
      impact.context_item_count += 1;
    }
  }

  return impact;
}

matters::Matter delete_chat(const std::string &project_id, const std::string &chat_id, long long expected_revision) {
  if (expected_revision < 1) throw std::runtime_error("This Chat revision is invalid.");
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");
  const auto found_chat = find_chat(*found_project, chat_id);
  if (found_chat == found_project->chats.end()) throw std::runtime_error("This Chat is no longer available.");
  if (found_chat->revision != expected_revision) {
    throw std::runtime_error("This Chat changed since you opened it. Review it and try again.");
  }
  // A real deletion, not the separate `archived` hide-from-list state. The
  // Chat's own MatterTask records are never touched here: they remain real
  // Project operation history (Usage/Routing Log keep working, and
  // WorkProductActions::provenanceFor() already resolves "Source Chat
  // deleted" honestly once no chat in project.chats names this task's
  // chat_id any more).
  // A tombstone, not an erased record: a turn another device sent into this
  // Chat before the deletion arrived stays reachable, and the Chat comes back
  // into the list so a person can restore it rather than losing that work.
  const std::string timestamp = workspaces::current_timestamp();
  found_chat->deleted_at = timestamp;
  found_chat->deleted_by_actor_id = platform::resolve_actor_identity().actor_id;
  found_chat->updated_at = timestamp;
  found_chat->updated_by_actor_id = found_chat->deleted_by_actor_id;
  ++found_chat->revision;
  found_project->updated_at = timestamp;
  matters::save_matter(workspace_path, *found_project);
  return *found_project;
}

matters::Matter restore(const std::string &project_id, const std::string &chat_id) {
  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");
  const auto found_chat = find_chat(*found_project, chat_id);
  if (found_chat == found_project->chats.end()) throw std::runtime_error("This Chat is no longer available.");
  if (found_chat->deleted_at.empty()) return *found_project;
  const std::string timestamp = workspaces::current_timestamp();
  found_chat->deleted_at.clear();
  found_chat->deleted_by_actor_id.clear();
  found_chat->updated_at = timestamp;
  found_chat->updated_by_actor_id = platform::resolve_actor_identity().actor_id;
  ++found_chat->revision;
  found_project->updated_at = timestamp;
  matters::save_matter(workspace_path, *found_project);
  return *found_project;
}

ChatCompactionResult compact(const std::string &project_id, const std::string &chat_id) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	const auto workspace_path = node::require_active_workspace_path();
	auto found_project = matters::load_matter(workspace_path, project_id);
	if (!found_project) throw std::runtime_error("This Project is no longer available.");
	const auto found_chat = find_chat(*found_project, chat_id);
	if (found_chat == found_project->chats.end()) throw std::runtime_error("This Chat is no longer available.");

	// This Chat's own turns, exactly as the next request would receive them -
	// the same selection and the same records, never a second reading of the
	// conversation (a retried turn is its latest attempt, a Consilium run its
	// synthesis, automatic work is not conversation).
	std::vector<matters::MatterTask> chat_tasks;
	for (const matters::MatterTask &task : found_project->tasks)
		if (task.chat_id && *task.chat_id == chat_id) chat_tasks.push_back(task);
	const auto items = conversation_history_items(*found_project, chat_tasks);
	if (items.empty()) throw std::runtime_error("This Chat has nothing to compact yet.");

	matters::ChatCompaction compaction;
	compaction.created_at = workspaces::current_timestamp();
	compaction.created_by_actor_id = platform::resolve_actor_identity().actor_id;
	std::size_t tokens_before = 0;
	nlohmann::ordered_json turns = nlohmann::ordered_json::array();
	nlohmann::ordered_json briefTurns = nlohmann::ordered_json::array();
	// A compaction that carried every turn's FULL record saved almost nothing -
	// it was the same records the assembler would have used, glued together
	// (measured: 3,505 -> 3,449 tokens, 1.6%). What a conversation needs from
	// its own past is not uniform: the last few turns are what a follow-up
	// actually builds on, and the older ones need to be NAMED, not re-read.
	//
	// So the newest covered turns keep their full record, and everything older
	// keeps its brief one - the request's opening, what became of it, and the
	// documents it cited. Still quoted, never re-written, still no model call:
	// every turn remains in the record and nothing is invented. What the
	// conversation depends on structurally (cited documents, work products,
	// artifacts, still-open work) is carried separately below, so brevity here
	// never costs a dependency.
	for (std::size_t index = 0; index < items.size(); ++index) {
		const context::PromptHistoryItem &item = items[index];
		tokens_before += research::estimate_tokens(item.content.empty() ? item.state_record : item.content);
		compaction.covered_task_ids.push_back(item.id);
		const bool amongTheNewest = items.size() - index <= kCompactionDetailedTurns;
		const std::string &kept = amongTheNewest || item.brief_record.empty()
			? item.state_record : item.brief_record;
		const nlohmann::json parsed = nlohmann::json::parse(kept, nullptr, false);
		if (parsed.is_discarded()) turns.push_back(nlohmann::ordered_json(kept));
		else turns.push_back(nlohmann::ordered_json(parsed));
		// The same conversation at its smallest, for a window that cannot hold
		// the one above. Every turn is still here.
		const std::string &briefest = item.brief_record.empty() ? item.state_record : item.brief_record;
		const nlohmann::json briefParsed = nlohmann::json::parse(briefest, nullptr, false);
		if (briefParsed.is_discarded()) briefTurns.push_back(nlohmann::ordered_json(briefest));
		else briefTurns.push_back(nlohmann::ordered_json(briefParsed));
	}
	// What the conversation still depends on, kept structured as well as quoted.
	for (const matters::MatterTask &task : chat_tasks) {
		if (task.chat_id == std::nullopt || *task.chat_id != chat_id) continue;
		for (const context::ResolvedSourceReference &reference : task.source_references)
			if (std::find(compaction.cited_document_ids.begin(), compaction.cited_document_ids.end(),
					reference.document_id) == compaction.cited_document_ids.end())
				compaction.cited_document_ids.push_back(reference.document_id);
		for (const std::string &id : task.result_work_product_ids)
			if (std::find(compaction.work_product_ids.begin(), compaction.work_product_ids.end(), id)
					== compaction.work_product_ids.end())
				compaction.work_product_ids.push_back(id);
		if (task.artifact_document_id
				&& std::find(compaction.artifact_document_ids.begin(), compaction.artifact_document_ids.end(),
					*task.artifact_document_id) == compaction.artifact_document_ids.end())
			compaction.artifact_document_ids.push_back(*task.artifact_document_id);
		if (task.direct_document_work_document_id
				&& std::find(compaction.artifact_document_ids.begin(), compaction.artifact_document_ids.end(),
					*task.direct_document_work_document_id) == compaction.artifact_document_ids.end())
			compaction.artifact_document_ids.push_back(*task.direct_document_work_document_id);
		// Work this conversation left open is the first thing a person loses to
		// a summary, so it is carried explicitly rather than hoped for.
		const std::string outcome = turn_outcome(task);
		if (outcome == "waiting_for_approval" || outcome == "failed" || outcome == "stopped_by_user"
				|| outcome == "in_progress" || outcome == "conflict" || outcome == "interrupted")
			compaction.unresolved.push_back(text_opening(task.prompt, kBriefTurnRecordRequestBytes)
				+ " [" + outcome + "]");
	}

	nlohmann::ordered_json record{{"conversationSoFar", std::move(turns)}};
	if (!compaction.unresolved.empty()) record["stillOpen"] = compaction.unresolved;
	compaction.record = record.dump();
	nlohmann::ordered_json briefRecord{{"conversationSoFar", std::move(briefTurns)}};
	// What the conversation still has open is never the thing that yields to a
	// small window: it is carried in both forms.
	if (!compaction.unresolved.empty()) briefRecord["stillOpen"] = compaction.unresolved;
	compaction.brief_record = briefRecord.dump();
	compaction.covered_task_ids.shrink_to_fit();

	ChatCompactionResult result;
	result.covered_turns = items.size();
	result.tokens_before = tokens_before;
	result.tokens_after = research::estimate_tokens(compaction.record);
	result.tokens_after_brief = research::estimate_tokens(compaction.brief_record);

	found_chat->compaction = std::move(compaction);
	found_chat->updated_at = workspaces::current_timestamp();
	found_chat->updated_by_actor_id = platform::resolve_actor_identity().actor_id;
	++found_chat->revision;
	found_project->updated_at = found_chat->updated_at;
	matters::save_matter(workspace_path, *found_project);
	result.chat = *found_chat;
	return result;
}

std::vector<ChatSearchMatch> search(const std::string &project_id, const std::string &query) {
  const std::string trimmed_query = trim(query);
  if (trimmed_query.empty()) return {};
  const std::string folded_query = casefold(trimmed_query);

  const persistence::PersistenceLock lock(persistence::persistence_mutex());
  const auto workspace_path = node::require_active_workspace_path();
  auto found_project = matters::load_matter(workspace_path, project_id);
  if (!found_project) throw std::runtime_error("This Project is no longer available.");

  std::vector<ChatSearchMatch> results;
  for (const matters::MatterChat &chat : found_project->chats) {
    if (chat.archived == true) continue;

    const std::string folded_title = casefold(chat.title);
    const auto title_position = folded_title.find(folded_query);
    if (title_position != std::string::npos) {
      results.push_back(ChatSearchMatch{chat.id, chat.title, "title",
          build_snippet(chat.title, title_position, folded_query.size())});
      continue;
    }

    bool matched = false;
    for (const matters::MatterTask &task : found_project->tasks) {
      if (matched) break;
      if (!task.chat_id || *task.chat_id != chat.id || task.operation_type != "matter_task") continue;

      const std::string folded_prompt = casefold(task.prompt);
      const auto prompt_position = folded_prompt.find(folded_query);
      if (prompt_position != std::string::npos) {
        results.push_back(ChatSearchMatch{chat.id, chat.title, "prompt",
            build_snippet(task.prompt, prompt_position, folded_query.size())});
        matched = true;
        break;
      }

      if (task.response_text) {
        const std::string folded_response = casefold(*task.response_text);
        const auto response_position = folded_response.find(folded_query);
        if (response_position != std::string::npos) {
          results.push_back(ChatSearchMatch{chat.id, chat.title, "response",
              build_snippet(*task.response_text, response_position, folded_query.size())});
          matched = true;
          break;
        }
      }
    }
  }
  return results;
}


namespace {

std::mutex &read_marks_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::filesystem::path read_marks_path() { return platform::app_settings_directory() / "ReadMarks.json"; }

bool is_mark_key(const std::string &value) {
  if (value.empty() || value.size() > 120) return false;
  return std::all_of(value.begin(), value.end(), [](char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' || character == '.' || character == ':'
        || character == '-';
  });
}

bool is_mark_time(const std::string &value) {
  if (value.empty() || value.size() > 40) return false;
  return std::all_of(value.begin(), value.end(), [](char character) {
    return std::isdigit(static_cast<unsigned char>(character)) != 0 || character == 'T' || character == 'Z' || character == ':'
        || character == '.' || character == '+' || character == '-';
  });
}

ReadMarksPlace &installed_read_marks_place() {
	static ReadMarksPlace place;
	return place;
}

// A member's folder of marks in the firm's record: their actor id with every
// letter no folder name may hold written as its hex, so it is one folder for
// one member on every system.
std::string marks_folder_name(const std::string &actor_id) {
	static const char *const digits = "0123456789abcdef";
	std::string name;
	for (const char character : actor_id) {
		const auto byte = static_cast<unsigned char>(character);
		if (std::isalnum(byte) != 0 || character == '_' || character == '-') {
			name += character;
		} else {
			name += '.';
			name += digits[byte >> 4];
			name += digits[byte & 0x0F];
		}
	}
	return name.substr(0, 160);
}

std::optional<std::filesystem::path> shared_marks_folder(const std::string &actor_id) {
	const ReadMarksPlace &place = installed_read_marks_place();
	if (!place.firm) return std::nullopt;
	try {
		const std::optional<std::filesystem::path> firm = place.firm();
		if (firm.has_value() == false) return std::nullopt;
		return *firm / ".lm" / "ReadMarks" / marks_folder_name(actor_id);
	} catch (const std::exception &) {
		return std::nullopt;
	}
}

std::string marks_device() {
	const ReadMarksPlace &place = installed_read_marks_place();
	std::string device = place.device_id ? place.device_id() : std::string();
	if (device.empty() || device.size() > 80 || std::all_of(device.begin(), device.end(), [](char character) {
			return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' || character == '-';
		}) == false) {
		device = "device";
	}
	return device;
}

// One file of marks, as one computer wrote it.
std::map<std::string, std::string> marks_in_file(const std::filesystem::path &path) {
	std::map<std::string, std::string> marks;
	std::ifstream input(path, std::ios::binary);
	if (!input) return marks;
	try {
		const nlohmann::json kept = nlohmann::json::parse(input);
		const auto listed = kept.find("marks");
		if (listed == kept.end() || !listed->is_object()) return marks;
		for (const auto &[key, value] : listed->items())
			if (value.is_string() && is_mark_key(key) && is_mark_time(value.get<std::string>())) marks[key] = value.get<std::string>();
	} catch (const std::exception &) {
	}
	return marks;
}

void keep_later(std::map<std::string, std::string> &marks, const std::map<std::string, std::string> &more) {
	for (const auto &[key, at] : more)
		if (marks[key] < at) marks[key] = at;
}

// Every member's marks as kept; a file that cannot be read is no marks.
nlohmann::json load_read_marks_file() {
  std::ifstream input(read_marks_path(), std::ios::binary);
  if (!input) return nlohmann::json::object();
  try {
    nlohmann::json kept = nlohmann::json::parse(input);
    return kept.is_object() ? kept : nlohmann::json::object();
  } catch (const std::exception &) {
    return nlohmann::json::object();
  }
}

std::map<std::string, std::string> marks_of(const nlohmann::json &kept, const std::string &actor_id) {
  std::map<std::string, std::string> marks;
  const auto members = kept.find("members");
  if (members == kept.end() || !members->is_object()) return marks;
  const auto member = members->find(actor_id);
  if (member == members->end() || !member->is_object()) return marks;
  for (const auto &[key, value] : member->items())
    if (value.is_string() && is_mark_key(key) && is_mark_time(value.get<std::string>())) marks[key] = value.get<std::string>();
  return marks;
}

} // namespace

// Every mark of the member: the latest of every computer's file in the firm's
// record, and of what was kept here before it.
std::map<std::string, std::string> all_marks_of(const std::string &actor_id) {
  std::map<std::string, std::string> marks = marks_of(load_read_marks_file(), actor_id);
  if (const std::optional<std::filesystem::path> folder = shared_marks_folder(actor_id)) {
    std::error_code error;
    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(*folder, error)) {
      if (entry.path().extension() == ".json") keep_later(marks, marks_in_file(entry.path()));
    }
  }
  return marks;
}

void install_read_marks_place(ReadMarksPlace place) {
  const std::lock_guard<std::mutex> lock(read_marks_mutex());
  installed_read_marks_place() = std::move(place);
}

std::map<std::string, std::string> read_marks(const std::string &actor_id) {
  const std::lock_guard<std::mutex> lock(read_marks_mutex());
  return all_marks_of(actor_id);
}

std::map<std::string, std::string> mark_read(const std::string &actor_id, const std::string &conversation, const std::string &up_to) {
  if (actor_id.empty()) throw std::invalid_argument("No member is signed in at this computer.");
  if (!is_mark_key(conversation)) throw std::invalid_argument("That conversation cannot be marked read.");
  if (!is_mark_time(up_to)) throw std::invalid_argument("That time cannot mark a conversation read.");
  const std::lock_guard<std::mutex> lock(read_marks_mutex());
  std::map<std::string, std::string> marks = all_marks_of(actor_id);
  std::string &mark = marks[conversation];
  if (mark >= up_to) return marks;
  mark = up_to;
  // The oldest marks go first beyond 4,000: a conversation not seen for so
  // long reads as new again, never wrongly as read.
  constexpr std::size_t kMostMarks = 4000;
  while (marks.size() > kMostMarks) {
    auto oldest = marks.begin();
    for (auto entry = marks.begin(); entry != marks.end(); ++entry)
      if (entry->second < oldest->second) oldest = entry;
    marks.erase(oldest);
  }
  // This computer writes only its own file: the firm's record's where it is,
  // the member's preferences here otherwise.
  const std::optional<std::filesystem::path> folder = shared_marks_folder(actor_id);
  std::filesystem::path target;
  nlohmann::json content;
  if (folder.has_value() == true) {
    target = *folder / (marks_device() + ".json");
    content = nlohmann::json{{"formatVersion", 1}, {"marks", marks}};
  } else {
    target = read_marks_path();
    content = load_read_marks_file();
    if (!content.contains("members") || !content["members"].is_object()) content["members"] = nlohmann::json::object();
    content["version"] = 1;
    content["members"][actor_id] = marks;
  }
  std::error_code error;
  std::filesystem::create_directories(target.parent_path(), error);
  // Staged beside it under a name the exchange never carries, then put in place.
  const std::filesystem::path staged = target.parent_path() / (".lm-tmp-" + target.filename().string());
  {
    std::ofstream output(staged, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Singularity could not keep what you have read on this computer.");
    output << content.dump();
    if (!output) throw std::runtime_error("Singularity could not keep what you have read on this computer.");
  }
  std::filesystem::rename(staged, target, error);
  if (error) throw std::runtime_error("Singularity could not keep what you have read on this computer.");
  if (folder.has_value() == true && installed_read_marks_place().written) installed_read_marks_place().written();
  return marks;
}
} // namespace singularity::chat
