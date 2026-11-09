#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "Platform/HttpClient/HttpClient.hpp"

namespace singularity::node {

// Signing in to the Legal Machines account a person's organisation holds: the
// seat they were given, confirmed with its password and, when the organisation
// asks for it, a one-time code. It is what lets this computer download the
// models Singularity needs (LocalModel/ModelPackageManager) - never where any of
// the person's work goes.
//
// The password is sent once, over HTTPS, to Legal Machines only, and is never
// kept, logged or traced by anything here. No reply carries a secret back.

// The transport: an HTTPS POST of one JSON body to a Legal Machines address,
// injectable so every answer the service can give is tested without a network.
// Throws platform::HttpTransportError (or std::runtime_error) when no answer
// came at all.
using AccountPoster = std::function<platform::HttpResponse(const std::string &url, const std::string &json_body)>;

enum class SignInStatus {
	SignedIn,
	TwoFactorRequired,
	EmailVerificationRequired,
	// A new organisation that Legal Machines has not approved yet: nothing
	// is kept and nothing is downloaded until it is.
	AwaitingApproval,
	// A new organisation whose setup is not finished: the password was right,
	// and its setup goes on with two-factor authentication.
	SetupRequired,
};

const char *to_string(SignInStatus status);

// What the service said to a sign-in that it accepted.
struct SignInAnswer {
	SignInStatus status = SignInStatus::SignedIn;
	// With TwoFactorRequired: what the code must be sent back with, and where
	// the code comes from - "authenticator" for the seat's authenticator app,
	// "email" for one mailed. With SignedIn, a colleague's seat's sign-in
	// that stands checked, which this computer's session is asked against;
	// empty otherwise.
	std::string challenge_id;
	std::string method = "email";
	// Set only with SetupRequired: what the setup sets next ("two_factor"), and
	// the token that opens it, which stays native.
	std::string setup;
	std::string setup_token;
	// The organisation's contact address as the service masks it
	// ("le***@example.com"), when it said one.
	std::optional<std::string> masked_email;
	bool is_contact_verified = false;
	// How the account names the person and their firm, when it does.
	std::optional<std::string> display_name;
	std::optional<std::string> organisation_name;
	// The organisation and the seat a sign-in by email opened, as the service
	// named them.
	std::optional<std::string> organisation_id;
	std::optional<std::string> seat_id;
	// The seat's role as the service said it: "admin" for the firm's
	// administrator seat, "member" for a colleague's seat it made.
	std::optional<std::string> role;
	// A colleague's seat with no authenticator yet: signed in, and asked
	// to set one up before it works.
	bool needs_two_factor_setup = false;
	// The second step was a recovery code: the seat signed in without its
	// authenticator app, and has this many codes left.
	bool used_recovery_code = false;
	std::optional<int> recovery_codes_left;
};

enum class AccountFailureKind {
	InvalidInput,
	WrongCredentials,
	WrongCode,
	TooManyAttempts,
	ServiceUnavailable,
	Offline,
	Refused,
	// The download session needs a sign-in code checked first: sign in again.
	CodeRequired,
	// The organisation's email is not confirmed yet, so no session is issued.
	OrganisationNotActive,
	// Legal Machines has not approved the organisation yet, so no session is
	// issued.
	AwaitingApproval,
	// An organisation with these details is registered already.
	AlreadyRegistered,
	// A new organisation's setup can no longer be finished from here: sign in
	// to go on with it.
	SetupExpired,
	// The seat has no authenticator app yet, and its session opens nothing but
	// setting one up: set up two-step sign-in, then the same call works.
	TwoStepSetupRequired,
	// The administrator's password reset took the email's code, and the seat
	// signs in with two steps: the same reset goes on with the code its
	// authenticator app shows, or a recovery code. Nothing was used up.
	TwoStepCodeRequired,
};

// A sign-in the service did not accept, or could not be asked. Its message is
// the sentence a person reads - what happened and what to do - and never
// contains the password, the code or anything the service returned as a secret.
class AccountFailure : public std::runtime_error {
public:
	AccountFailure(AccountFailureKind kind, const std::string &message)
		: std::runtime_error(message)
		, kind_(kind)
	{
	}

	AccountFailureKind getKind() const { return kind_; }

	// The stable code the interface says this failure by (NativeError).
	std::string code() const {
		switch (kind_) {
			case AccountFailureKind::InvalidInput: return "account.invalid_input";
			case AccountFailureKind::WrongCredentials: return "account.wrong_credentials";
			case AccountFailureKind::WrongCode: return "account.wrong_code";
			case AccountFailureKind::TooManyAttempts: return "account.too_many_attempts";
			case AccountFailureKind::ServiceUnavailable: return "account.service_unavailable";
			case AccountFailureKind::Offline: return "account.offline";
			case AccountFailureKind::Refused: return "account.refused";
			case AccountFailureKind::CodeRequired: return "account.code_required";
			case AccountFailureKind::OrganisationNotActive: return "account.organisation_not_active";
			case AccountFailureKind::AwaitingApproval: return "account.awaiting_approval";
			case AccountFailureKind::AlreadyRegistered: return "account.already_registered";
			case AccountFailureKind::SetupExpired: return "account.setup_expired";
			case AccountFailureKind::TwoStepSetupRequired: return "account.two_step_setup_required";
			case AccountFailureKind::TwoStepCodeRequired: return "account.two_step_code_required";
		}
		return "account.refused";
	}

private:
	AccountFailureKind kind_;
};

// A seat with no authenticator app may do nothing at Legal Machines but set one
// up: its session reads its own seat and sets up two steps, and every other
// door - the firm's seats and computers, the models - answers 403 with
// "twoFactorSetup": "required". Such an answer is never a session to drop or a
// password to ask again: the seat sets up two-step sign-in, and the same
// session then opens everything.
inline constexpr const char *kTwoStepSetupFirst = "Set up two-step sign-in first. Then try again.";
bool is_two_step_setup_refusal(const platform::HttpResponse &response);
// Tells the window that the seat must set up two-step sign-in, as the event
// account.twoStepSetupRequired; at most once in ten seconds, since the firm's
// computers are asked every few.
void tell_two_step_setup_required();

// The identifiers as the service knows them: without surrounding spaces and in
// small letters. Throws AccountFailure(InvalidInput) for one that cannot be an
// identifier at all, before anything is sent.
std::string normalized_organisation_id(const std::string &organisation_id);
std::string normalized_seat_id(const std::string &seat_id);

// The organisation's email as the service knows it: without surrounding spaces
// and in small letters. Throws AccountFailure(InvalidInput) for text that
// cannot be an address at all, before anything is sent.
std::string normalized_email(const std::string &email);

// POST {base}/auth/login {organisationId, seatId, password}.
SignInAnswer sign_in(const AccountPoster &post,
                     const std::string &organisation_id,
                     const std::string &seat_id,
                     const std::string &password);

// POST {base}/organisations/lookup {email}: whether Legal Machines knows an
// organisation by this email, its contact address.
bool organisation_exists(const AccountPoster &post, const std::string &email);

// The same lookup, whole: whether an organisation uses this email, the one
// every seat of the firm signs in with.
struct EmailLookup {
	bool exists = false;
};
EmailLookup look_up_email(const AccountPoster &post, const std::string &email);

// A firm's seats, each a login of the firm's with no person in it: its seat
// id, drawn by Legal Machines from a random set, and a password of its own.
// Posted with this computer's session, which must be the firm's
// administrator's; each throws AccountFailure in words for the lawyer.
struct FirmSeat {
	std::string seat_id;
	std::string role;
	std::string created_at;
	// Whether it signs in with an authenticator's code.
	bool has_two_factor = false;
};
// POST {base}/organisations/seats {}: the firm's seats, the administrator's
// first, and the firm's own email they sign in with, when the service says it.
struct FirmSeats {
	std::vector<FirmSeat> seats;
	std::optional<std::string> organisation_email;
};
FirmSeats list_seats(const AccountPoster &session_post);
// POST {base}/organisations/seats/create {seatId?, password}: a colleague's
// seat, which signs in by the firm's email, its seat id and this password; its
// seat id - the one the app drew and showed (empty: Legal Machines draws it).
std::string create_seat(const AccountPoster &session_post, const std::string &seat_id, const std::string &password);
// POST {base}/organisations/seats/password {seatId, password}: a colleague's
// seat's new password, which signs that seat's computers out.
void set_seat_password(const AccountPoster &session_post, const std::string &seat_id, const std::string &password);
// POST {base}/organisations/seats/remove {seatId}: a colleague's seat goes.
void remove_seat(const AccountPoster &session_post, const std::string &seat_id);
// POST {base}/organisations/seats/two-factor/reset {seatId}: a colleague's
// two-step sign-in turned off, for a lost phone; they set it again at their
// next sign-in.
void reset_seat_two_factor(const AccountPoster &session_post, const std::string &seat_id);

// This computer's own seat, with its session, any seat's.
// - own_seat: its id, its role and whether two-step sign-in is on;
// - start_own_two_factor: the key an authenticator app takes, kept until a
//   code turns it on; confirm_own_two_factor: the code the app shows, which
//   turns it on and answers the ten recovery codes, shown once;
// - change_own_password: with the password it has now and, with two-step on,
//   a code the app shows or a recovery code.
struct OwnSeat {
	std::string seat_id;
	std::string role;
	bool has_two_factor = false;
	// How many recovery codes are left unused; none where the service does
	// not say.
	std::optional<int> recovery_codes_left;
	// Its firm is one of Legal Machines' design partners, a mark Legal
	// Machines alone sets: the seat may report problems and share work logs.
	// False where the service does not say.
	bool is_design_partner = false;
	// The version of the Diagnostics Consent this seat accepted, or none: a
	// design partner's seat sends nothing to Legal Machines before it.
	std::optional<std::string> diagnostics_consent;
};
OwnSeat own_seat(const AccountPoster &session_post);
// The seat accepts this version of the Diagnostics Consent: Legal Machines
// keeps it on the seat with when. Throws where it is not kept, so that nothing
// is sent without it.
void accept_diagnostics_consent(const AccountPoster &session_post, const std::string &version);
struct OwnTwoFactorKey {
	std::string secret;
	std::string uri;
};
OwnTwoFactorKey start_own_two_factor(const AccountPoster &session_post);
std::vector<std::string> confirm_own_two_factor(const AccountPoster &session_post, const std::string &code);
void change_own_password(const AccountPoster &session_post, const std::string &current, const std::string &wanted,
	const std::string &code);

// Two-step sign-in changed by the rules every service keeps, each only once
// the seat proved it is itself - its password and a code its app shows now, or
// a recovery code, which is then used up.
// - replace_own_two_factor: a new authenticator app's key, kept beside the one
//   in use until confirm_replaced_two_factor's code from the new app swaps
//   them and answers ten new recovery codes; until then the old app and codes
//   work, and an attempt left halfway changes nothing;
// - renew_own_recovery_codes: ten new recovery codes; the old stop at once.
//   There is no turning it off: every seat signs in with two steps.
OwnTwoFactorKey replace_own_two_factor(const AccountPoster &session_post, const std::string &password, const std::string &code);
std::vector<std::string> confirm_replaced_two_factor(const AccountPoster &session_post, const std::string &code);
std::vector<std::string> renew_own_recovery_codes(const AccountPoster &session_post, const std::string &password,
	const std::string &code);

// The firm's administrator who forgot their password, with no session, by the
// firm's email: request_recovery mails a code to it when it is a firm's
// confirmed email, with the same answer when it is not (POST
// {base}/organisations/recover {email}); reset_password takes that code, with
// two-step on also a code the authenticator shows or a recovery code, and the
// new password, which signs every computer of the seat out (POST
// {base}/organisations/reset-password). Sent with no two-step code, a reset
// whose email code is right fails with TwoStepCodeRequired where two steps are
// on, so the code that changes every 30 seconds is asked for last, on its own.
void request_recovery(const AccountPoster &post, const std::string &email);
void reset_password(const AccountPoster &post, const std::string &email, const std::string &email_code,
	const std::string &two_step_code, const std::string &wanted);

// The firm's computers, with this computer's session, any seat's: public
// keys and addresses only, which nothing here trusts until a lawyer has
// checked the computers' number.
struct ListedComputer {
	std::string seat_id;
	// "administrator" or "member": the seat's office, never who sits in it.
	std::string role;
	std::string device_id;
	std::string device_key;
	std::string coordination_key;
	std::string signature;
	std::vector<std::string> addresses;
	std::string announced_at;
};
// POST {base}/organisations/computers/announce: this computer's device key,
// its X25519 key signed by the device key, and where its direct link may be
// reached.
void announce_computer(const AccountPoster &session_post, const ListedComputer &self);
// POST {base}/organisations/computers {}: the firm's computers, newest first.
std::vector<ListedComputer> list_computers(const AccountPoster &session_post);
// The check two computers make of each other, passed on by Legal Machines and
// nothing more: send a "commit" or a "reveal", and read what waits for this
// computer, once.
struct PairingMessage {
	std::string from_device;
	std::string kind;
	std::string value;
};
void send_pairing(const AccountPoster &session_post, const std::string &from_device, const std::string &to_device,
		const std::string &kind, const std::string &value);
std::vector<PairingMessage> pairing_inbox(const AccountPoster &session_post, const std::string &device_id);

// POST {base}/auth/login {email, seat?, password}: signing in by the
// organisation's email and a seat's id instead of its Organisation and Seat
// IDs; with no seat, the administrator's. The answer names the organisation
// and the seat the password opened.
SignInAnswer sign_in_with_email(const AccountPoster &post, const std::string &email, const std::string &seat,
		const std::string &password);

// A new organisation, as the person registering it gives it. Each agreement is
// the person's own acceptance of a legal document, never assumed.
struct OrganisationRegistration {
	std::string legal_entity_name;
	std::string country;
	std::string registration_number;
	std::string contact_email;
	std::string password;
	bool accepts_licence_and_terms = false;
	bool acknowledges_privacy_notice = false;
	bool consents_to_personal_data = false;
	bool confirms_authority = false;
	bool confirms_shared_mailbox = false;
	bool wants_product_updates = false;
};

// What the service made of a registration: the organisation, its first seat,
// and where the code that confirms its email went.
struct RegistrationAnswer {
	std::string organisation_id;
	std::string seat_id;
	std::optional<std::string> masked_email;
};

// POST {base}/organisations/register-start, then
// {base}/organisations/register-complete. Legal Machines names the
// organisation, and mails a code to its email.
RegistrationAnswer register_organisation(const AccountPoster &post, const OrganisationRegistration &registration);

// POST {base}/organisations/create: a new organisation from its details and
// agreements alone, no password. Legal Machines names it and mails the code
// alone to its email; the setup that follows the code sets the password and
// two-factor authentication. The registration's password is not read.
RegistrationAnswer create_organisation(const AccountPoster &post, const OrganisationRegistration &registration);

// What confirming the organisation's email came to: it waits for Legal
// Machines' approval, or it is in use at once.
enum class ContactConfirmation {
	AwaitingApproval,
	Active,
};

const char *to_string(ContactConfirmation confirmation);

// What confirming the email came to, and for a new organisation the setup
// it opens: what it sets first ("password" or "two_factor") and the token
// that opens it. The token stays native.
struct ContactConfirmed {
	ContactConfirmation state = ContactConfirmation::AwaitingApproval;
	std::string setup;
	std::string setup_token;
};

// POST {base}/organisations/verify-contact {organisationId, code}.
ContactConfirmed confirm_contact(const AccountPoster &post, const std::string &organisation_id,
                                 const std::string &code);

// A new organisation's setup, each step opened by the token the email's
// confirmation or a sign-in gave. A token that opens nothing throws
// AccountFailure(SetupExpired).
//
// POST {base}/organisations/setup/password: 12 characters at least.
void set_initial_password(const AccountPoster &post, const std::string &organisation_id,
                          const std::string &setup_token, const std::string &password);

// POST {base}/organisations/setup/two-factor: the key an authenticator app
// takes, typed by hand or scanned from its otpauth link.
struct TwoFactorKey {
	std::string secret;
	std::string uri;
};
TwoFactorKey start_two_factor(const AccountPoster &post, const std::string &organisation_id,
                              const std::string &setup_token);

// POST {base}/organisations/setup/two-factor/confirm: the code the app shows
// turns two-factor on and ends the setup; the answer carries the ten recovery
// codes, shown once.
struct TwoFactorOn {
	ContactConfirmation state = ContactConfirmation::AwaitingApproval;
	std::vector<std::string> recovery_codes;
	// Active at the end of its setup, the administrator stands signed in for
	// this computer: the service's verified sign-in, which the app claims at
	// once with the password just set, and the organisation's name. Empty
	// where the service gives none: the app then signs in with a code.
	std::string challenge_id;
	std::optional<std::string> organisation_name;
};
TwoFactorOn confirm_two_factor(const AccountPoster &post, const std::string &organisation_id,
                               const std::string &setup_token, const std::string &code);

// POST {base}/organisations/resend-contact-code {organisationId}: a new code
// to the email that waits to be confirmed.
void resend_contact_code(const AccountPoster &post, const std::string &organisation_id);

// POST {base}/auth/2fa/verify {challengeId, code}: the six-digit code - mailed,
// or shown by the seat's authenticator app - or a recovery code's eight letters
// and digits. Returns, once the code is accepted, a signed-in answer with the
// names the service gives: the organisation's and the person's (none from an
// older service, or for a seat whose name it does not know yet); throws
// AccountFailure otherwise.
SignInAnswer verify_two_factor(const AccountPoster &post, const std::string &challenge_id, const std::string &code);

// POST {base}/organisations/contact-status {organisationId}: whether Legal
// Machines has approved the organisation, which the app's wait for it asks
// while it is shown. Throws AccountFailure when the service cannot say.
bool organisation_is_approved(const AccountPoster &post, const std::string &organisation_id);

// POST {base}/auth/node-session {organisationId, seatId, password, nodeId,
// challengeId?}: exchanges the seat's password for this computer's model
// download session and returns the session token. `challenge_id` names the
// sign-in whose code was just checked (empty when the sign-in had no code);
// the service issues the session against that check alone. The caller keeps
// the token native-only (Provider/CredentialStore); nothing here keeps, traces
// or says it, and a failure's words never carry it or the password.
std::string exchange_node_session(const AccountPoster &post,
                                  const std::string &organisation_id,
                                  const std::string &seat_id,
                                  const std::string &password,
                                  const std::string &node_id,
                                  const std::string &challenge_id = "");

} // namespace singularity::node
