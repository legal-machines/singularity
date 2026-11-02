#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// A colleague's talk (the owner, 2026-10-26; D-129): every message to an AI
// colleague is answered first by one light call that carries who the colleague
// is (its role, D-101's second layer), how a colleague talks in a chat, its
// tools on a line, and the conversation's latest messages - nothing of any
// Matter. "The model decides, and we give it as much data as it asks for": what
// the colleague needs it asks for with a tool and gets only that, earlier
// messages too, and work on the documents it takes on with `work`, which the
// assignment loop then carries out with the full brief (D-121). "Hi!" costs one
// call of a few hundred tokens ("a couple of hundred at most", the owner).
//
// Pure: the model and what the tools reach are given by the caller, so a test
// runs a whole conversation without a provider or a Matter. The prompt only
// grows within a turn - each round appends what was looked up - so a provider
// that caches a prompt's beginning reuses all of it.
namespace singularity::document_review::talk {

// One message of the conversation, as the colleague reads it.
struct TalkLine {
	// Who wrote it, as the firm names them; the colleague's own say "You".
	std::string author;
	// When, as the record has it ("2026-10-26T00:06:58Z").
	std::string at;
	std::string text;
	bool isOwn = false;
	// The message's id in its thread, for a reaction to it; empty for a line
	// that is no message (a run's own reason).
	std::string id;
	// What its words name among the Matter's documents, read on this computer
	// before anything is masked, one phrase a line: "\"Harborline NDA\" is
	// DOC_24 \"Harborline Mutual NDA.docx\" - the same words stand in the names
	// of ...". Masking sends the person's words and the document's name as two
	// symbols that say nothing of each other. Empty when its words name none.
	std::string names;
	// What the message carries besides its words, as its thread shows it: the
	// files it attaches and the place in a document it is about ("It is about
	// section 2.1 (Use and Disclosure) of “…”, which reads: “…”"). An ask from
	// the Office pane is its words with its place: shown its words alone, a
	// colleague did not know which copy of the NDA the lawyer was in
	// (2026-10-26). Empty for a message with neither.
	std::string carries;
	// The document its place is in, where it has one: what its words name is
	// read with that document first.
	std::string placeDocumentId;
};

// A message of the colleague's other conversations a new message may need
// (D-203): where it was said - a thread's title, or the member's personal
// chat -, by whom, when, and its words.
struct RecallCandidate {
	std::string where;
	std::string author;
	std::string at;
	std::string text;
};

// What recall brings: where, who, when, and the words around what it shares
// with the new message.
struct RecalledPassage {
	std::string where;
	std::string author;
	std::string at;
	std::string text;
	double score = 0;
};

struct TalkInput {
	// Who the colleague is in the firm (Agent/ColleagueBrief's identity): its
	// office, practice and area - of the whole firm, never of one Matter (the
	// owner, 2026-10-26: "she exists within the whole firm"). Always the first
	// thing sent.
	std::string brief;
	// What it remembers that it may use here (D-159): its own notes, one memory
	// across every conversation it has in the firm, numbered "M1", "M2" so it
	// can forget one; empty when it has none it may use here. It follows the
	// brief, so every talk of the colleague shares the brief's cache.
	std::string memory;
	std::string agentName;
	// The member the colleague talks with: their name, their office
	// ("Senior Associate, Labor & Employment") and where they stand to the
	// colleague ("who ranks below you"), each empty where the firm does not say.
	std::string personName;
	std::string personRole;
	std::string personStanding;
	// The member's personal chat with the colleague (D-042), or a thread of a
	// Matter, named by the Matter and the thread's title.
	bool isPersonalChat = true;
	std::string matterName;
	std::string threadTitle;
	// Oldest first. What came after the colleague's own latest message is new -
	// what it answers; what came before was said already, its own words too.
	// Only the latest messages are sent (talkCall); `history` gives the rest.
	std::vector<TalkLine> conversation;
	// The number the conversation's first message has in its thread (1 for
	// the thread's first): each line is shown with its number, the same in
	// every talk, so the colleague can react to any of them.
	std::size_t firstNumber = 1;
	// The new message went to everyone (@all) and did not name the colleague:
	// it answers only if it has something to add; a reaction, or nothing, does.
	bool isToAll = false;
	// What its other conversations hold for the new message, found here by the
	// words they share before the model is called (D-203): sent after the new
	// message, so what comes before it stays the provider's cache.
	std::vector<RecalledPassage> recalled;
	// Whether the talk keeps its notes in its own answer ("remember" and
	// "forget", D-159). A colleague whose memory a model of its own gathers
	// after it answered (D-206) is asked for none: its notes are only used.
	bool asksMemory = true;
};

// The candidates most worth bringing to `query`, found here with no model:
// ranked by the words they share with it (BM25 over the candidates: a rarer
// word counts more, and a name, a figure or a section number most), the newer
// first at an equal score; nothing that shares no word that counts. At most
// `most_passages`, each cut to about `passage_characters` around what it
// shares, all together at most `total_characters`; the same words never twice.
std::vector<RecalledPassage> recall(const std::string& query, const std::vector<RecallCandidate>& candidates,
	std::size_t most_passages = 4, std::size_t passage_characters = 280, std::size_t total_characters = 1100);

// The conversation a talk carries: what is new (at most kMostNewLines), and
// before it the messages said already, at least kTalkWindowLeast of them. The
// window begins at a multiple of kTalkWindowStep, so it stays where it is for
// several messages and the provider's cache of it holds; it then carries up to
// kTalkWindowLeast + kTalkWindowStep - 1 messages said already.
constexpr std::size_t kTalkWindowLeast = 4;
constexpr std::size_t kTalkWindowStep = 8;
constexpr std::size_t kMostNewLines = 6;
// Where the window begins for a conversation whose new messages begin at
// `firstNew`.
std::size_t talkWindowStart(std::size_t firstNew);
// Where a conversation's new messages begin: after the colleague's own latest
// message, and at most kMostNewLines before its end.
std::size_t firstNewLine(const std::vector<TalkLine>& conversation);

// What a tool gives back: its words for the colleague, or why it gives nothing.
// A document's passages come whole, each as the Matter cut it, after `text`:
// each goes as a part of its own, so what Papinian Guard found in it when the
// document became ready is reused, and it is not scored again.
struct ToolAnswer {
	ToolAnswer() = default;
	ToolAnswer(std::string words, std::string why = {}, std::vector<std::string> parts = {})
		: text(std::move(words)), refused(std::move(why)), passages(std::move(parts)) {}

	std::string text;
	std::string refused;
	std::vector<std::string> passages;
};

// What the colleague's tools reach, given by the caller. Every one only reads;
// each is called with the arguments the colleague gave, as text.
struct TalkSources {
	std::function<ToolAnswer()> matters;
	std::function<ToolAnswer(const std::string& matter)> documents;
	std::function<ToolAnswer(const std::string& document, long long from)> read;
	std::function<ToolAnswer(const std::string& matter, const std::string& words)> find;
	std::function<ToolAnswer(const std::string& matter)> threads;
	std::function<ToolAnswer()> record;
	std::function<ToolAnswer(const std::string& need)> abilities;
	// The conversation's earlier messages, a few at a time from message `from`
	// (0 the first); runTalk gives them from the conversation itself where this
	// is unset.
	std::function<ToolAnswer(long long from)> history;
	// Told once, when the colleague first looks something up: it is at work
	// from then on, and `said` is what it tells them as it starts ("Let me
	// check the NDA."), empty where it said nothing (the owner, 2026-10-26: "if
	// the model asks for tools, switch to work, say a phrase first - better
	// from the API too - work, then give the final answer").
	std::function<void(const std::string& said)> working;
};

// A note the colleague keeps from the conversation, as it says it (D-159): of
// a kind - rule, preference, fact, decision or commitment - about the firm,
// the conversation's Matter, or the member it talks with ("person").
struct TalkMemory {
	std::string kind;
	std::string about;
	std::string text;
};

// One call of the model: its answer in words, or why there is none. The words
// are the colleague's message, and a line "CALL <tool> {fields}" for each tool
// it calls (readAnswer).
struct TalkAsked {
	bool isAnswered = false;
	std::string text;
	std::string failure;
	// The model cannot be asked now for a cause that clears by itself (D-169):
	// the talk is interrupted, never failed, and asked again later.
	bool isUnavailable = false;
	long long inputTokens = 0;
	long long outputTokens = 0;
	long long reasoningTokens = 0;
	// Of the input, what the provider read from its cache, and wrote to it.
	long long cachedInputTokens = 0;
	long long cacheWriteTokens = 0;
	long long costMicros = 0;
	long long milliseconds = 0;
	int calls = 0;
};

// One call of the talk: its prompt in parts, joined in order - the parts the
// same in every talk of this colleague first (who it is, the rules, the
// tools), then the conversation said already one message a part, then what is
// new, then what each round looked up - so what was protected and cached of a
// part is reused. The conversation said already only grows from one talk to
// the next while its window stays, so the next talk reads it from the cache.
struct TalkCall {
	// How a colleague talks here and its tools: Singularity's own words, the
	// same in every talk of every colleague, read before the parts. Sent as the
	// turn's instruction, which protection passes as Singularity's own, never
	// as data: a line's word taken for a name ("colleague") once rewrote the
	// rules themselves ("a conversation with a [[Person3]] of the firm",
	// 2026-10-26).
	std::string instruction;
	std::vector<std::string> parts;
	// Parts [0, stableParts) are the same in every talk of this colleague.
	std::size_t stableParts = 0;
	// Parts [0, conversationParts) end with the conversation's last message
	// said already (its header, where there is none).
	std::size_t conversationParts = 0;
	// Parts [0, newParts) end with what is new.
	std::size_t newParts = 0;
	// Parts [0, previousParts) are what the round before sent; 0 in the first
	// round. A provider finds what an earlier call kept only near where this
	// one asks it to keep (twenty blocks back at most), so a later round marks
	// where the round before ended: its third round once found none of its
	// second, a passage a part, and paid to keep it all again (2026-10-26).
	std::size_t previousParts = 0;
	// Counts from 1.
	int round = 1;
	// An answer that says nothing, reacts to nothing and calls nothing is an
	// answer (TalkInput::isToAll).
	bool maySayNothing = false;
	// The answer's shape holds "remember" and "forget" (TalkInput::asksMemory).
	bool asksMemory = true;
};

// The model, asked with the call; it answers in words. The caller asks again
// where an answer cannot be used.
using TalkModel = std::function<TalkAsked(const TalkCall& call)>;

struct TalkOutcome {
	// "said": answered in words, or with reactions alone; "work": the colleague
	// takes work on, `said` being what it says as it does; "silent": a message
	// to everyone it had nothing to add to; "failed": no answer could be had;
	// "interrupted": the model could not be asked now, for a cause that clears
	// by itself (D-169) - asked again later, nothing said meanwhile.
	std::string status = "failed";
	std::string said;
	std::string work;
	// The Matter the work is for, as the colleague named it in a personal chat,
	// where no Matter is the conversation's own (D-157); empty otherwise.
	std::string workMatter;
	std::string failure;
	// What it told them as it started looking things up (TalkSources::working);
	// empty where it looked nothing up, or said nothing as it began.
	std::string started;
	// The message those words were posted as, where the caller posted them;
	// empty otherwise. Taking the work on, that message is its word on it.
	std::string startedMessageId;
	// Its reactions, with its words or in their place: each the message's id
	// and the reaction (talkReactions) - to any message the talk showed it but
	// its own (the owner, 2026-10-26: "they may react to any message of the
	// chat, economically - they hold its whole context anyway"). At most
	// kMostReactions.
	std::vector<std::pair<std::string, std::string>> reactions;
	// What it keeps in its memory from this conversation, at most
	// kMostRemembered, and the notes it forgets ("M3"), within the answer it
	// gives anyway: no call of its own (D-159).
	std::vector<TalkMemory> remembered;
	std::vector<std::string> forgotten;
	int rounds = 0;
	int calls = 0;
	// Each tool the colleague called, in order, by name, and how many of its
	// calls were refused (a document not found, a look already made, too many
	// at once) - counts and names only, for measuring.
	std::vector<std::string> toolsUsed;
	int refusedCalls = 0;
	long long inputTokens = 0;
	long long outputTokens = 0;
	long long reasoningTokens = 0;
	long long cachedInputTokens = 0;
	long long cacheWriteTokens = 0;
	long long costMicros = 0;
	long long milliseconds = 0;
	// The prompt of each round, for a test to compare.
	std::vector<std::string> prompts;
};

// The most rounds of looking up one message may take before the colleague
// must answer with what it has.
constexpr int kMostTalkRounds = 6;
// The most reactions one answer puts.
constexpr std::size_t kMostReactions = 3;
// The most notes one talk keeps, and the most characters of one.
constexpr std::size_t kMostRemembered = 3;
constexpr std::size_t kMostMemoryNoteCharacters = 300;
// The most tools one round may call.
constexpr std::size_t kMostCallsPerRound = 4;

// The first round's prompt: its instruction - how a colleague talks here and
// the tools - and its parts: the brief, then the conversation a message a part.
TalkCall talkCall(const TalkInput& input);

// A round's prompt as the model reads it: the instruction, then the parts.
std::string promptOf(const TalkCall& call);

// The first round's prompt as one text, as the model reads it.
std::string talkPrompt(const TalkInput& input);

// A message exactly as a talk's conversation carries it, with its number in
// the thread, a part of its own: what Papinian Guard scores of it, and finds
// again the next time.
std::string lineFor(const TalkLine& line, std::size_t number);

// The answer's shape: one JSON object, "say" its message, "react" its
// reaction, "calls" the tools it calls now, and "remember" and "forget" what it
// keeps in its memory and what it lets go (D-159). Asked of the model, it keeps an
// answer that does not think on its task: without it a model with no thinking
// read Singularity's envelope as the question (2026-10-26).
nlohmann::json talkSchema();
// The same, with no "remember" or "forget" where the talk keeps no notes of
// its own (TalkInput::asksMemory, D-206).
nlohmann::json talkSchema(bool asksMemory);

// What a colleague keeps of a talk, gathered by a model of its own once it
// has answered (D-206): its notes that hold here, numbered as in the talk,
// and the exchange - what was new to it, and what it answered. Asked as a
// short call: the notes to keep, at most kMostRemembered, and the numbers of
// notes the exchange shows wrong.
struct MemoryAsk {
	std::string agentName;
	// TalkInput::memory: "M1 ...", one note a line; empty for none.
	std::string memory;
	std::string personName;
	bool isPersonalChat = true;
	std::string matterName;
	// What was new to it, oldest first.
	std::vector<TalkLine> exchange;
	// Its answer, or what it said as it took the work on, and the work.
	std::string answer;
};
struct MemoryCall {
	// Singularity's own words: how to keep a colleague's memory.
	std::string instruction;
	// The notes and the exchange, as material.
	std::string prompt;
};
MemoryCall memoryCall(const MemoryAsk& ask);
// {"remember": [{kind, about, text}], "forget": ["M3"]}.
nlohmann::json memorySchema();
struct MemoryAnswer {
	std::vector<TalkMemory> remembered;
	std::vector<std::string> forgotten;
};
// The memory call's answer read as the talk's own notes are read: a note of a
// known kind about the firm, the Matter or the person, cut to
// kMostMemoryNoteCharacters, at most kMostRemembered, no two alike; a number
// "M<n>". Nothing for words that are no such object.
std::optional<MemoryAnswer> readMemoryAnswer(const std::string& words);

// The reactions a colleague puts on a message: the app's own quick reactions
// (thumbs up, check mark, eyes, folded hands, question mark), as UTF-8.
const std::vector<std::string>& talkReactions();

// An answer read: {"say": its message, "react": [{"to": a line's number,
// "with": a reaction}], "calls": [{"tool": ..., its fields}]} - from that JSON
// object, or from words where a line "CALL <tool> {fields as JSON}" is a call
// and every other line the message. A reaction not of talkReactions() is left
// out.
nlohmann::json readAnswer(const std::string& text);

// Why an answer cannot be used, for the caller to ask again; empty when it
// can: it says nothing, reacts to nothing and calls no tool - unless it may.
std::string whyUnusable(const std::string& text, bool maySayNothing = false);

TalkOutcome runTalk(const TalkInput& input, const TalkSources& sources, const TalkModel& model);

// The key that keeps a conversation's calls on one endpoint of the provider,
// where its cache is (OpenRouter's session_id): `random` makes it the first
// time the conversation is asked for, and it is the same for that conversation
// while Singularity runs. Nothing of the conversation is in it: `conversation`
// only finds it again.
std::string sessionFor(const std::string& conversation, const std::function<std::string()>& random);

} // namespace singularity::document_review::talk
