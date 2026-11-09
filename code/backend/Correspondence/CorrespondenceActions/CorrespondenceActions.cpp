#include "Correspondence/CorrespondenceActions/CorrespondenceActions.hpp"

#include "Document/DocumentActions/DocumentActions.hpp"
#include "DocumentIntelligence/EmailDraft/EmailDraft.hpp"
#include "Matter/MatterRepository/MatterRepository.hpp"
#include "Node/ActiveWorkspace/ActiveWorkspace.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Office/OfficePaths/OfficePaths.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppPaths/AppPaths.hpp"
#include "Platform/AtomicFile/AtomicFile.hpp"
#include "Platform/Events/Events.hpp"
#include "Platform/SecureRandom/SecureRandom.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

namespace singularity::correspondence {
namespace {

constexpr std::size_t kMostTextBytes = 200000;
constexpr std::size_t kMostAttachments = 50;
constexpr std::size_t kMostParties = 100;
constexpr std::size_t kMostEvidence = 8;
constexpr std::size_t kMostAttachmentBytes = 30 * 1024 * 1024;

std::string lowered(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return text;
}

matters::Matter matter_here(const std::filesystem::path &workspace, const std::string &matter_id) {
	const std::optional<matters::Matter> matter = matters::load_matter(workspace, matter_id);
	if (!matter) throw CorrespondenceRefused("That Matter is not open on this computer.");
	return *matter;
}

void announce(const std::string &matter_id) {
	platform::events::publish("matter.changed", nlohmann::json{{"projectId", matter_id}});
}

std::string person_name(const platform::ActorIdentity &identity) {
	if (identity.display_name.has_value() && !one_line(*identity.display_name, 200).empty()) return *identity.display_name;
	return "A member";
}

Party kept_party(const Party &party) {
	return Party{one_line(party.name, 200), one_line(party.address, 320)};
}

// The letter as it is kept: its parts on their limits, its direction from the
// mailbox it is filed from.
Letter letter_of(const MessageFacts &facts) {
	if (one_line(facts.internet_message_id, 1000).empty()) {
		throw CorrespondenceRefused("Only a letter that was sent or received is filed; this one has no Message-ID yet.");
	}
	if (facts.attachments.size() > kMostAttachments) throw CorrespondenceRefused("A letter is filed with at most 50 attachments.");
	if (facts.to.size() > kMostParties || facts.cc.size() > kMostParties) {
		throw CorrespondenceRefused("A letter is filed with at most 100 recipients on each line.");
	}
	Letter letter;
	letter.internet_message_id = one_line(facts.internet_message_id, 1000);
	letter.conversation_id = one_line(facts.conversation_id, 1000);
	letter.mailbox = one_line(facts.mailbox, 320);
	letter.subject = one_line(facts.subject, 500);
	letter.from = kept_party(facts.from);
	for (const Party &party : facts.to) letter.to.push_back(kept_party(party));
	for (const Party &party : facts.cc) letter.cc.push_back(kept_party(party));
	letter.sent_at = one_line(facts.sent_at, 64);
	letter.text = cut_text(facts.text, kMostTextBytes);
	for (const LetterAttachment &attachment : facts.attachments) {
		LetterAttachment kept;
		kept.name = one_line(attachment.name, 255);
		kept.content_type = one_line(attachment.content_type, 200);
		kept.size = attachment.size < 0 ? 0 : attachment.size;
		kept.provider_id = one_line(attachment.provider_id, 2000);
		letter.attachments.push_back(std::move(kept));
	}
	letter.direction = !letter.mailbox.empty() && address_of(letter.from) == lowered(letter.mailbox) ? "outgoing" : "incoming";
	return letter;
}

Letter *letter_in(matters::Matter &matter, const std::string &letter_id) {
	for (Letter &letter : matter.correspondence) {
		if (letter.id == letter_id) return &letter;
	}
	return nullptr;
}

} // namespace

Suggestion suggest(const MessageFacts &facts) {
	std::vector<matters::Matter> here;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		for (const std::string &id : matters::list_matter_ids(workspace)) {
			try {
				if (auto matter = matters::load_matter(workspace, id)) here.push_back(std::move(*matter));
			} catch (const std::exception &) {
				// A Matter that cannot be read now is no evidence either way.
			}
		}
	}
	return suggest_among(here, facts);
}

Letter file_letter(const std::string &matter_id, const MessageFacts &facts, const std::vector<std::string> &evidence,
	const std::string &filed_from, bool by_rule) {
	Letter letter = letter_of(facts);
	const platform::ActorIdentity identity = platform::resolve_actor_identity();
	letter.id = "letter_" + platform::secure_random_hex(8);
	letter.filing.by = by_rule ? "rule" : "person";
	letter.filing.actor_id = identity.actor_id;
	letter.filing.actor_name = person_name(identity);
	letter.filing.at = workspaces::current_timestamp();
	letter.filing.from = one_line(filed_from, 40);
	for (const std::string &line : evidence) {
		const std::string sentence = one_line(line, 300);
		if (!sentence.empty() && letter.filing.evidence.size() < kMostEvidence) letter.filing.evidence.push_back(sentence);
	}
	letter.updated_at = letter.filing.at;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		matters::Matter matter = matter_here(workspace, matter_id);
		if (const Letter *filed = letter_with_message_id(matter, letter.internet_message_id)) return *filed;
		// Filed by its intake only where one of its rules answers to the letter.
		if (by_rule) {
			const IntakeRule *rule = intake_rule_for(matter, facts);
			if (rule == nullptr) throw CorrespondenceRefused("No rule of " + matter.name + "'s correspondence intake files this letter.");
			letter.filing.evidence.insert(letter.filing.evidence.begin(), "Filed by a rule of its intake: letters to " + rule->mailbox
				+ " from " + rule->from + ".");
			if (letter.filing.evidence.size() > kMostEvidence) letter.filing.evidence.resize(kMostEvidence);
		}
		for (const std::string &id : matters::list_matter_ids(workspace)) {
			if (id == matter_id) continue;
			std::optional<matters::Matter> other;
			try {
				other = matters::load_matter(workspace, id);
			} catch (const std::exception &) {
				continue;
			}
			if (other && letter_with_message_id(*other, letter.internet_message_id) != nullptr) {
				throw CorrespondenceRefused("This letter is filed in " + other->name + ". Move it from there instead.");
			}
		}
		matter.correspondence.push_back(letter);
		matters::save_matter(workspace, matter);
	}
	announce(matter_id);
	return letter;
}

Letter move_letter(const std::string &matter_id, const std::string &letter_id, const std::string &to_matter_id) {
	if (matter_id == to_matter_id) throw CorrespondenceRefused("The letter is filed in that Matter already.");
	const platform::ActorIdentity identity = platform::resolve_actor_identity();
	Letter moved;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		matters::Matter from = matter_here(workspace, matter_id);
		matters::Matter to = matter_here(workspace, to_matter_id);
		Letter *letter = letter_in(from, letter_id);
		if (letter == nullptr) throw CorrespondenceRefused("That letter is no longer filed in this Matter.");
		moved = *letter;
		const std::string now = workspaces::current_timestamp();
		moved.filing.evidence.insert(moved.filing.evidence.begin(), "Moved here from " + from.name + " by " + person_name(identity)
			+ "; first filed there by " + (letter->filing.actor_name.empty() ? std::string("a member") : letter->filing.actor_name)
			+ " on " + day_of(letter->filing.at) + ".");
		if (moved.filing.evidence.size() > kMostEvidence) moved.filing.evidence.resize(kMostEvidence);
		moved.filing.by = "person";
		moved.filing.actor_id = identity.actor_id;
		moved.filing.actor_name = person_name(identity);
		moved.filing.at = now;
		moved.updated_at = now;
		from.correspondence.erase(std::remove_if(from.correspondence.begin(), from.correspondence.end(),
			[&](const Letter &entry) { return entry.id == letter_id; }), from.correspondence.end());
		if (const Letter *there = letter_with_message_id(to, moved.internet_message_id)) {
			moved = *there;
		} else {
			to.correspondence.push_back(moved);
			matters::save_matter(workspace, to);
		}
		matters::save_matter(workspace, from);
	}
	announce(matter_id);
	announce(to_matter_id);
	return moved;
}

void unfile_letter(const std::string &matter_id, const std::string &letter_id) {
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		matters::Matter matter = matter_here(workspace, matter_id);
		const auto before = matter.correspondence.size();
		matter.correspondence.erase(std::remove_if(matter.correspondence.begin(), matter.correspondence.end(),
			[&](const Letter &entry) { return entry.id == letter_id; }), matter.correspondence.end());
		if (matter.correspondence.size() == before) throw CorrespondenceRefused("That letter is no longer filed in this Matter.");
		matters::save_matter(workspace, matter);
	}
	announce(matter_id);
}

namespace {

// The email files of a Matter's folder already read here, by Matter, place and
// bytes: each is read once a run of the program, whatever it turned out to be.
std::mutex &read_emails_mutex() {
	static std::mutex mutex;
	return mutex;
}

std::set<std::string> &read_emails() {
	static std::set<std::string> read;
	return read;
}

std::string email_key(const std::string &matter_id, const matters::MatterDocument &document) {
	return matter_id + "|" + document.relative_path + "|" + document.fingerprint;
}

bool is_email_file(const matters::MatterDocument &document) {
	return document.available && !document.relative_path.empty()
		&& lowered(std::filesystem::u8path(document.relative_path).extension().u8string()) == ".eml";
}

// Whether the Matter's folder holds an email not yet read here: the window
// lists the letters at every reload, and a reload with none reads nothing.
bool has_unread_email(const matters::Matter &matter) {
	const std::lock_guard<std::mutex> lock(read_emails_mutex());
	for (const matters::MatterDocument &document : matter.documents) {
		if (is_email_file(document) && read_emails().count(email_key(matter.id, document)) == 0) return true;
	}
	return false;
}

} // namespace

std::vector<Letter> file_folder_letters(const std::string &matter_id) {
	matters::Matter matter;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		matter = matter_here(node::require_active_workspace_path(), matter_id);
	}
	if (!matter.directory_path || matter.directory_path->empty()) return {};
	const std::filesystem::path folder = std::filesystem::u8path(*matter.directory_path);
	const platform::ActorIdentity identity = platform::resolve_actor_identity();
	// The mailboxes letters of this Matter were filed from: an email sent from
	// one of them is outgoing, as is one from the member at this computer.
	std::set<std::string> mailboxes;
	for (const Letter &letter : matter.correspondence) {
		if (!letter.mailbox.empty()) mailboxes.insert(lowered(letter.mailbox));
	}
	std::vector<Letter> found;
	std::vector<std::string> keys;
	for (const matters::MatterDocument &document : matter.documents) {
		if (!is_email_file(document)) continue;
		const std::string key = email_key(matter_id, document);
		{
			const std::lock_guard<std::mutex> lock(read_emails_mutex());
			if (read_emails().count(key) > 0) continue;
		}
		// Read once: what is not a letter is never read again; a letter, once it
		// is filed or known to be filed.
		const auto settled = [&key]() {
			const std::lock_guard<std::mutex> lock(read_emails_mutex());
			read_emails().insert(key);
		};
		const document_intelligence::ReadEmail read = document_intelligence::readEmailFile(folder / std::filesystem::u8path(
			document.relative_path));
		if (!read.isRead) {
			settled();
			continue;
		}
		EmailFile email;
		email.is_unsent = read.isUnsent;
		email.message_id = read.messageId;
		email.in_reply_to = read.inReplyTo;
		email.references = read.references;
		email.from = read.from;
		email.to = read.to;
		email.cc = read.cc;
		email.date = read.date;
		email.subject = read.subject;
		email.text = read.body;
		for (const auto &[name, bytes] : read.attachments) email.attachments.push_back({name, static_cast<long long>(bytes.size())});
		std::optional<MessageFacts> facts = facts_of_email(email);
		if (!facts || letter_with_message_id(matter, facts->internet_message_id) != nullptr) {
			settled();
			continue;
		}
		const bool isOurs = mailboxes.count(lowered(facts->from.address)) > 0
			|| (identity.display_name.has_value() && !facts->from.name.empty()
				&& lowered(one_line(*identity.display_name, 200)) == lowered(facts->from.name));
		if (isOurs) facts->mailbox = facts->from.address;
		Letter letter;
		try {
			letter = letter_of(*facts);
		} catch (const CorrespondenceRefused &) {
			settled();
			continue;
		}
		// The same email filed on two computers is one letter: its id is made
		// from its Message-ID, never drawn at random.
		letter.id = "letter_" + platform::sha256_hex_digest("folder:" + lowered(letter.internet_message_id)).substr(0, 16);
		letter.provider = "folder";
		letter.filing.by = "person";
		letter.filing.at = workspaces::current_timestamp();
		letter.filing.from = "folder";
		letter.filing.evidence.push_back(one_line("Put in the Matter's folder as “" + document.relative_path + "”.", 300));
		letter.updated_at = letter.filing.at;
		found.push_back(std::move(letter));
		keys.push_back(key);
	}
	if (found.empty()) return {};

	std::vector<Letter> filed;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		matters::Matter current = matter_here(workspace, matter_id);
		std::vector<matters::Matter> others;
		for (const std::string &id : matters::list_matter_ids(workspace)) {
			if (id == matter_id) continue;
			try {
				if (auto other = matters::load_matter(workspace, id)) others.push_back(std::move(*other));
			} catch (const std::exception &) {
			}
		}
		for (Letter &letter : found) {
			if (letter_with_message_id(current, letter.internet_message_id) != nullptr) continue;
			// A letter filed in another Matter here stays there; it is moved, never
			// filed twice.
			const bool isElsewhere = std::any_of(others.begin(), others.end(), [&](const matters::Matter &other) {
				return letter_with_message_id(other, letter.internet_message_id) != nullptr;
			});
			if (isElsewhere) continue;
			current.correspondence.push_back(letter);
			filed.push_back(letter);
		}
		if (!filed.empty()) matters::save_matter(workspace, current);
	}
	{
		const std::lock_guard<std::mutex> lock(read_emails_mutex());
		for (const std::string &key : keys) read_emails().insert(key);
	}
	if (!filed.empty()) announce(matter_id);
	return filed;
}

ReplyDraft reply_draft(const Letter &letter, const std::string &text) {
	ReplyDraft draft;
	if (letter.direction == "outgoing") {
		if (letter.to.empty() == false) draft.to = letter.to.front().address;
	} else {
		draft.to = letter.from.address;
	}
	std::string subject = letter.subject;
	const auto first = subject.find_first_not_of(" \t");
	subject = first == std::string::npos ? std::string() : subject.substr(first);
	std::string lead = subject.substr(0, 3);
	for (char &character : lead) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	draft.subject = lead == "re:" ? subject : "Re: " + subject;
	draft.body = text;
	return draft;
}

namespace {

// A name the draft's file may have on every system: the subject, without the
// characters a file name cannot hold.
std::string draft_file_name(const std::string &subject) {
	std::string name;
	for (const char character : subject) {
		const bool isRefused = std::string("<>:\"/\\|?*").find(character) != std::string::npos
			|| static_cast<unsigned char>(character) < 32;
		const char kept = isRefused ? ' ' : character;
		if (kept == ' ' && name.empty() == false && name.back() == ' ') continue;
		name += kept;
	}
	const auto first = name.find_first_not_of(" .");
	const auto last = name.find_last_not_of(" .");
	name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
	if (name.size() > 80) name.resize(80);
	return name.empty() ? "Reply" : name;
}

} // namespace

ReplyFile write_reply_with_files(const std::string &matter_id, const Letter &letter, const std::string &text,
	const std::vector<std::string> &document_ids) {
	const ReplyDraft reply = reply_draft(letter, text);
	if (reply.to.empty()) throw CorrespondenceRefused("This letter names no address to reply to. Reply to it from your mail app.");
	document_intelligence::EmailDraft draft;
	draft.to = reply.to;
	draft.subject = reply.subject;
	draft.body = reply.body;
	for (const std::string &document_id : document_ids) {
		std::filesystem::path file;
		try {
			file = document::file_of(matter_id, document_id);
		} catch (const std::exception &) {
			throw CorrespondenceRefused("A file of this reply is no longer in the Matter. Take it off the reply, then try again.");
		}
		draft.attachments.push_back({file, file.filename().u8string()});
	}
	const std::filesystem::path folder = platform::application_data_directory() / "Mail drafts";
	std::error_code error;
	std::filesystem::create_directories(folder, error);
	// Each draft in a folder of its own, so its file keeps the subject's name.
	const auto week_ago = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 7);
	for (std::filesystem::directory_iterator entry(folder, error), end; error.value() == 0 && entry != end; entry.increment(error)) {
		std::error_code time_error;
		if (entry->is_directory(time_error) && entry->last_write_time(time_error) < week_ago) {
			std::error_code remove_error;
			std::filesystem::remove_all(entry->path(), remove_error);
		}
	}
	const std::filesystem::path own = folder / platform::secure_random_hex(8);
	std::filesystem::create_directories(own, error);
	const std::filesystem::path target = own / std::filesystem::u8path(draft_file_name(reply.subject) + ".eml");
	const document_intelligence::EmailDraftOutcome written = document_intelligence::writeEmailDraft(target, draft);
	if (written.isWritten == false) {
		throw CorrespondenceRefused("The draft with its files could not be made: " + written.problem);
	}
	std::vector<std::filesystem::path> files;
	for (const document_intelligence::EmailAttachment &attachment : draft.attachments) files.push_back(attachment.path);
	return ReplyFile{target, written.attached, files};
}

std::vector<Letter> letters_of(const std::string &matter_id) {
	matters::Matter matter;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		matter = matter_here(node::require_active_workspace_path(), matter_id);
	}
	// An email a member put in the Matter's folder is one of its letters, filed
	// the first time it is seen here.
	if (has_unread_email(matter)) {
		try {
			if (!file_folder_letters(matter_id).empty()) {
				const persistence::PersistenceLock lock(persistence::persistence_mutex());
				matter = matter_here(node::require_active_workspace_path(), matter_id);
			}
		} catch (const std::exception &) {
		}
	}
	std::vector<Letter> letters = matter.correspondence;
	std::stable_sort(letters.begin(), letters.end(), [](const Letter &one, const Letter &other) {
		return one.sent_at > other.sent_at;
	});
	return letters;
}

LetterAttachment save_attachment(const std::string &matter_id, const std::string &letter_id, const std::string &provider_id,
	const std::string &bytes) {
	if (bytes.empty()) throw CorrespondenceRefused("The attachment came with no bytes.");
	if (bytes.size() > kMostAttachmentBytes) throw CorrespondenceRefused("An attachment is saved up to 30 MB.");
	matters::Matter matter;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		matter = matter_here(node::require_active_workspace_path(), matter_id);
	}
	const Letter *letter = letter_in(matter, letter_id);
	if (letter == nullptr) throw CorrespondenceRefused("That letter is not filed in this Matter.");
	const auto attachment = std::find_if(letter->attachments.begin(), letter->attachments.end(),
		[&](const LetterAttachment &entry) { return entry.provider_id == provider_id; });
	if (attachment == letter->attachments.end()) throw CorrespondenceRefused("That attachment is not one of the letter's.");
	if (!matter.directory_path || matter.directory_path->empty()) {
		throw CorrespondenceRefused("The Matter's folder is not on this computer, so the attachment was not saved.");
	}
	const std::filesystem::path folder = std::filesystem::u8path(*matter.directory_path);
	std::error_code error;
	if (!std::filesystem::is_directory(folder, error)) {
		throw CorrespondenceRefused("The Matter's folder is not reachable from this computer now, so the attachment was not saved.");
	}

	// The same bytes the Matter holds are that document, never a second copy.
	LetterAttachment saved = *attachment;
	saved.sha256 = platform::sha256_hex_digest(bytes);
	saved.was_already_there = false;
	for (const matters::MatterDocument &document : matter.documents) {
		if (!document.available || document.relative_path.empty() || lowered(document.fingerprint) != saved.sha256) continue;
		saved.relative_path = document.relative_path;
		saved.was_already_there = true;
		break;
	}
	if (!saved.was_already_there) {
		const std::filesystem::path target = office::free_path_in(folder, office::safe_file_name(saved.name));
		platform::write_file_durably(target, bytes);
		saved.relative_path = office::relative_inside(folder, target).value_or(target.filename().u8string());
	}

	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const std::filesystem::path workspace = node::require_active_workspace_path();
		matters::Matter current = matter_here(workspace, matter_id);
		Letter *kept = letter_in(current, letter_id);
		if (kept == nullptr) throw CorrespondenceRefused("That letter is no longer filed in this Matter.");
		for (LetterAttachment &entry : kept->attachments) {
			if (entry.provider_id != provider_id) continue;
			entry.sha256 = saved.sha256;
			entry.relative_path = saved.relative_path;
			entry.was_already_there = saved.was_already_there;
		}
		kept->updated_at = workspaces::current_timestamp();
		matters::save_matter(workspace, current);
	}
	announce(matter_id);
	return saved;
}

} // namespace singularity::correspondence
