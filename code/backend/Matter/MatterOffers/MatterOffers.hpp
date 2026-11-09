#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace singularity::matter::offers {

// A Matter given to a member of the firm by choosing them. The giving
// computer writes the offer into the two members' personal channel, which
// travels sealed to that member's computers a lawyer checked and to no one
// else: the Matter's name never passes through Legal Machines. The member's
// computer shows it - who gives what, and how much - and Accept takes the
// Matter from the giving computer, whose keys the two lawyers checked once,
// so nothing is read aloud.
struct Offer {
	std::string matter_id;
	std::string name;
	std::uint64_t files = 0;
	std::uint64_t bytes = 0;
	// Whose, and to whom: members, "seat:<id>".
	std::string from_actor;
	std::string to_actor;
	// The giving computer, and the Matter's invitation from it.
	std::string from_device;
	std::string from_device_key;
	std::string invitation;
	std::string at;
	// By the giving computer's device key, over signed_text().
	std::string signature;
};

using Verify = std::function<bool(const std::string &key, const std::string &message, const std::string &signature)>;

// What the giving computer signs: every field but the signature.
std::string signed_text(const Offer &offer);

// Where an offer of the Matter `matter_id` stands in a channel's folder.
std::string offer_path(const std::string &matter_id);

// Writes the offer, signed already, into the channel's folder. Throws for a
// Matter id that is not one.
void write_offer(const std::filesystem::path &channel_folder, const Offer &offer);

// The offers in a channel's folder, as written; an unreadable one is left out.
std::vector<Offer> read_offers(const std::filesystem::path &channel_folder);

// The computer an offer must come from, as a lawyer here checked it
// (Matter/FirmComputers): its member, its device key and its X25519 key.
struct Giver {
	std::string actor_id;
	std::string device_id;
	std::string device_key;
	std::string coordination_key;
	std::vector<std::string> addresses;
};

// Whether an offer is to `me` from that very computer, signed by its device
// key. The Matter is then taken from the computer as checked - its device and
// its X25519 key -, never as the offer's invitation names it: only where it
// may be reached is read from that.
bool holds(const Offer &offer, const std::string &me, const Giver &giver, const Verify &verify);

// The lawyer's answer to an offer, kept on this computer in `records`: by the
// Matter and the giving computer.
enum class Answer { None, Accepted, Declined };
Answer answer_of(const std::filesystem::path &records, const std::string &matter_id, const std::string &from_device);
void keep_answer(const std::filesystem::path &records, const std::string &matter_id, const std::string &from_device, Answer answer,
		const std::string &at);

} // namespace singularity::matter::offers
