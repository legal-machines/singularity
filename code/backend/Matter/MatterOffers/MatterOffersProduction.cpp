#include "Matter/MatterOffers/MatterOffersProduction.hpp"

#include "Matter/FirmComputers/FirmComputers.hpp"
#include "Node/LegacyWorkspace/LegacyWorkspace.hpp"
#include "Platform/ActorIdentity/ActorIdentity.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/DeviceKey/DeviceKey.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace singularity::matter::offers {

namespace {

namespace fs = std::filesystem;

Verify device_verify() {
	return [](const std::string &key, const std::string &message, const std::string &signature) {
		return platform::verify_device_signature(key, message, signature);
	};
}

// The firm's computers a lawyer here checked, as the givers an offer may come
// from.
std::vector<Giver> checked_givers() {
	std::vector<Giver> givers;
	for (const firm_computers::Computer &computer : firm_computers::checked()) {
		givers.push_back(Giver{"seat:" + computer.seat_id, computer.device_id, computer.device_key, computer.coordination_key,
			computer.addresses});
	}
	return givers;
}

} // namespace

Given give(const std::string &matter_id, const std::string &name, const fs::path &folder, const std::string &actor_id) {
	const platform::ActorIdentity identity = platform::resolve_actor_identity();
	if (actor_id.empty() == true || actor_id == identity.actor_id) throw std::invalid_argument("Choose a colleague to give the Matter to.");
	if (actor_id.rfind("seat:", 0) != 0) throw std::invalid_argument("A Matter is given to a lawyer of the firm, in their seat.");
	if (identity.actor_id.rfind("seat:", 0) != 0) throw std::invalid_argument("Sign in to your seat of the firm first.");
	(void)coordination::share_matter(matter_id, name, folder);
	const std::string invitation = coordination::offer_matter(matter_id, actor_id);
	const coordination::MatterSize size = coordination::matter_size(matter_id);
	const coordination::PersonalChannel channel = coordination::personal_channel(actor_id);
	Offer offer;
	offer.matter_id = matter_id;
	offer.name = name;
	offer.files = size.files;
	offer.bytes = size.bytes;
	offer.from_actor = identity.actor_id;
	offer.to_actor = actor_id;
	offer.from_device = identity.device_id;
	offer.from_device_key = platform::device_public_key();
	offer.invitation = invitation;
	offer.at = workspaces::current_timestamp();
	offer.signature = platform::sign_with_device_key(signed_text(offer));
	write_offer(channel.folder, offer);
	coordination::wake();
	Given given;
	for (const Giver &computer : checked_givers()) {
		if (computer.actor_id == actor_id) given.reaches = true;
	}
	return given;
}

std::vector<Waiting> waiting() {
	std::vector<Waiting> found;
	const std::string me = platform::resolve_actor_identity().actor_id;
	if (me.rfind("seat:", 0) != 0) return found;
	const std::vector<Giver> givers = checked_givers();
	if (givers.empty() == true) return found;
	const coordination::CoordinationStatus status = coordination::status();
	// A Matter this computer carries or asked for is past its offer.
	std::set<std::string> known;
	for (const coordination::LinkedMatter &matter : status.matters) known.insert(matter.matter_id);
	const fs::path records = platform::computer_records_directory();
	const Verify verify = device_verify();
	for (const coordination::LinkedMatter &channel : status.personal) {
		if (channel.state != "carried") continue;
		for (const Offer &offer : read_offers(channel.folder)) {
			if (known.count(offer.matter_id) > 0) continue;
			if (answer_of(records, offer.matter_id, offer.from_device) != Answer::None) continue;
			const auto giver = std::find_if(givers.begin(), givers.end(),
				[&offer](const Giver &computer) { return computer.device_id == offer.from_device; });
			if (giver == givers.end() || holds(offer, me, *giver, verify) == false) continue;
			found.push_back(Waiting{offer, *giver});
		}
	}
	std::sort(found.begin(), found.end(), [](const Waiting &first, const Waiting &second) { return first.offer.at > second.offer.at; });
	return found;
}

coordination::CoordinationStatus accept(const std::string &matter_id, const std::string &from_device, const fs::path &folder) {
	for (const Waiting &entry : waiting()) {
		if (entry.offer.matter_id != matter_id || entry.offer.from_device != from_device) continue;
		// The giving computer as checked here; only where it may be reached is
		// read from the offer's invitation, and only for that very computer.
		coordination::Invitation invitation;
		invitation.matter_id = entry.offer.matter_id;
		invitation.name = entry.offer.name;
		invitation.device_id = entry.giver.device_id;
		invitation.coordination_key = entry.giver.coordination_key;
		invitation.candidates = entry.giver.addresses;
		const std::optional<coordination::Invitation> said = coordination::read_invitation(entry.offer.invitation);
		if (said.has_value() == true && said->device_id == entry.giver.device_id && said->coordination_key == entry.giver.coordination_key) {
			for (const std::string &candidate : said->candidates) {
				if (std::find(invitation.candidates.begin(), invitation.candidates.end(), candidate) == invitation.candidates.end()) {
					invitation.candidates.push_back(candidate);
				}
			}
		}
		const coordination::CoordinationStatus status = coordination::take_offered_matter(invitation, entry.giver.device_key, folder);
		keep_answer(platform::computer_records_directory(), matter_id, from_device, Answer::Accepted, workspaces::current_timestamp());
		return status;
	}
	throw std::runtime_error("That Matter is no longer offered to you here.");
}

void decline(const std::string &matter_id, const std::string &from_device) {
	keep_answer(platform::computer_records_directory(), matter_id, from_device, Answer::Declined, workspaces::current_timestamp());
}

} // namespace singularity::matter::offers
