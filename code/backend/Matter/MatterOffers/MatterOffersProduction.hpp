#pragma once

#include "Matter/MatterCoordination/MatterCoordination.hpp"
#include "Matter/MatterOffers/MatterOffers.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace singularity::matter::offers {

// A Matter given to a member of the firm by choosing them, as the app does it:
// through this computer's coordination, its device key and the firm's
// computers a lawyer here checked (Matter/FirmComputers).

struct Given {
	// Whether the member has a computer a lawyer here checked, which the offer
	// reaches now; else it waits in the two members' channel until one is.
	bool reaches = false;
};
// Gives the Matter in `folder` to the member `actor_id`: carried to their
// checked computers as they ask (coordination::offer_matter), offered in the
// two members' personal channel, signed by this computer. Throws, in words
// for the lawyer, for this computer's own member or a member with no seat.
Given give(const std::string &matter_id, const std::string &name, const std::filesystem::path &folder, const std::string &actor_id);

// An offer to this computer's member that holds - from a computer a lawyer
// here checked, signed by it - and is neither answered nor taken.
struct Waiting {
	Offer offer;
	Giver giver;
};
std::vector<Waiting> waiting();

// Takes the offered Matter into `folder`, from the giving computer as checked.
// Throws when no such offer waits.
coordination::CoordinationStatus accept(const std::string &matter_id, const std::string &from_device, const std::filesystem::path &folder);
// Leaves it: the offer is no longer shown here.
void decline(const std::string &matter_id, const std::string &from_device);

} // namespace singularity::matter::offers
