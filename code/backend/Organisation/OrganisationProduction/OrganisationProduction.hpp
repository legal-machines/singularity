#pragma once

#include "Matter/SharedMatter/SharedMatterService.hpp"
#include "Organisation/FirmPeople/FirmPeople.hpp"

#include "Organisation/OrganisationReadModel/OrganisationReadModel.hpp"
#include "Organisation/Types/FirmOffice.hpp"

#include <functional>
#include <filesystem>
#include <string>

// The organisation read on this computer (Beta 1 #273): the facts gathered
// from the domains that own them, each through its public interface
// (docs/ARCHITECTURE.md, ARCH-8). Platform's identity and settings give the
// viewer, Matter's shared records and published agent cards give the people
// and the colleagues other computers run, Agent the colleagues run here, Chat
// the operations of each Matter, Matter its conflicts and overwritten changes.
// No domain depends on this one.
namespace singularity::organisation {

OrganisationFacts gather_facts();

// Who is looking, on this computer, and what today's rights let them do
// (Beta 1 #274): the member's identity and seat only, read without gathering
// the organisation. Every read the new frontend uses carries it.
Viewer viewer();

// The organisation as the window reads it: gather_facts(), built.
OrganisationView read();

// The place in the firm the person at this computer states - their position
// on the firm's scale and their practice (Organisation/Types/FirmOffice) -
// kept beside their Settings, as their name is. Until the organisation has a
// home of its own (R3, OPEN-1), each member states their own, and it travels
// in their member record into every Matter they open (Matter/SharedMatter),
// so a colleague knows who writes to it and where they stand.
MemberOffice own_office();
// Keeps it, after its words are checked, and writes it into this person's
// member record of every Matter known here at once. Throws
// std::invalid_argument with the sentence the person reads.
MemberOffice set_own_office(const MemberOffice &office);

// The photo the person at this computer shows the firm (D-163): a small
// square JPEG the window cut from a picture they chose, as a data URL, kept
// beside their office and travelling the same way, in their member record,
// into every Matter they open. It never goes to a Legal Machines server.
// Empty without one.
std::string own_photo();
// Keeps it (an empty one takes it away), once it is a photo
// (matter::is_member_photo), and writes it into this person's member record
// of every Matter known here at once. Throws std::invalid_argument with the
// sentence the person reads.
std::string set_own_photo(const std::string &photo);
// Who sits in the firm's seats (D-209; Organisation/FirmPeople), as this
// computer keeps them in step with the firm's record:
// - publish_own_profile: the member's own name, office and photo, said into
//   the firm's record for their seat when they changed; called by the member's
//   own edits;
// - adopt_own_profile: a newer profile of this computer's own seat, written by
//   the administrator or by the member elsewhere, taken here as the member's
//   own name, office and photo, once;
// - save_profile_of_seat: a seat's profile, saved by the firm's administrator
//   for any seat or by the member for their own; throws std::invalid_argument
//   with the sentence the person reads;
// - firm_team_with_profiles: the firm's team (Matter/SharedMatter firm_team)
//   with what the firm's record says of each seat laid over it, and a person
//   for each seat no Matter here names yet.
void publish_own_profile();
void adopt_own_profile();
// The firm's AI Setup (D-210; Organisation/FirmPeople): an administrator's
// computer says its own to the firm's record when it changed; a member's
// computer takes the firm's model for each role whose account is connected
// here - given by the firm's computers - and keeps its own for any other.
// Never throws.
void adopt_firm_ai_setup();

// A colleague the firm removed (D-214) is no longer one of its computers'
// peers: this computer refuses the computers it knew as theirs - met in a
// Matter or checked for the firm (D-205) - so nothing more of a Matter goes to
// them and nothing of theirs is taken. Final for those keys
// (Tool/DeviceTrust). Run as the team is read, so each computer of the firm
// does it once the firm's record says so.
void refuse_removed_colleagues(const matter::FirmTeam &team);
SeatProfile save_profile_of_seat(const SeatProfile &wanted);
// A colleague's seat archived, restored or removed (D-214), by the firm's
// administrator, never their own: `state` is "archived", "" (restored) or
// "removed". Legal Machines is told first through `at_legal_machines` - the
// seat closed, or made again - unless it was closed already, then the firm's
// record says where they stand. Throws std::invalid_argument with the sentence
// the person reads.
SeatProfile set_seat_state(const std::string &seat_id, const std::string &state, const std::function<void()> &at_legal_machines);
matter::FirmTeam firm_team_with_profiles();

// The picture in a file the person chose, as a data URL the window cuts their
// photo from: a JPEG, PNG, GIF or WebP of at most 20 MB. Nothing is kept.
// Throws std::invalid_argument with the sentence the person reads.
std::string picture_from_file(const std::filesystem::path &path);

} // namespace singularity::organisation
