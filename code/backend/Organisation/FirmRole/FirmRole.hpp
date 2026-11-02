#pragma once

#include <string>

// The role of the person at this computer in the firm (D-184): its
// administrative account, which hires the firm's AI colleagues, sets each
// one's model and provider and connects the model accounts; or a member who
// joined the firm with an invitation, who does none of these (the owner,
// 2026-10-31). Rights and checks belong to the role (PHIL-2).
//
// Legal Machines' account service does not say which seat answers for the
// firm, so this computer records the role where it is certain, in "Member
// Role.json" beside the member's office, written only by the native account
// flow, never by the window:
// - joining the firm with an invitation makes a member;
// - registering the firm makes its administrator, from the end of its setup;
// - a later sign-in keeps the role recorded for the same seat; a seat with no
//   role recorded here is a member, so a new computer never starts with the
//   administrator's rights;
// - a computer nobody has joined to a firm, or one signed in before roles were
//   recorded, is its own administrator, so today's installations keep working.
// A Debug build started for a whole-product test (D-182) takes the role from
// the test: NATIVE_APP_EMULATE_ACCOUNT=register is the administrator, =join a
// member.
namespace singularity::organisation {

enum class FirmRole { Administrator, Member };

FirmRole firm_role();
bool is_administrator();

// Recorded by the account flow when a seat joins the firm with an invitation,
// or when the firm's own registration completes.
void record_joined(const std::string &seat_id);
void record_registered(const std::string &seat_id);

// Recorded by the account flow when a seat signs in: the role already
// recorded for the same seat stands; any other seat is a member.
void record_signed_in(const std::string &seat_id);

// Recorded when Legal Machines says the seat's role as it signs in (D-204):
// the firm's administrator seat, or a colleague's seat it made.
void record_role(const std::string &seat_id, FirmRole role);

std::string to_string(FirmRole role);

} // namespace singularity::organisation
