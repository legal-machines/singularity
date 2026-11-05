#pragma once

#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "Node/LegalMachinesAccount/LegalMachinesAccount.hpp"

namespace singularity::bridge::account_bridge {

// What signing in reaches outside the bridge, injectable so the whole flow is
// tested without a network, a credential store or a download.
struct AccountServices {
	node::AccountPoster post;
	// Starts signing this computer in for model downloads and downloading the
	// protection model, off the calling thread, and returns at once. It gets its
	// own copy of the password, which is dropped when that work ends, and the
	// sign-in whose code was just checked (empty when it had none); its progress
	// is papinian.getModelStatus.
	std::function<void(std::string password, std::string challenge_id)> start_authorization;
	// Whether this computer holds a download session, and forgetting it. Never
	// the session itself.
	std::function<bool()> has_session;
	std::function<void()> forget_session;
	// Posts to Legal Machines with this computer's session (its bearer and
	// whose it is): the account's own requests, such as inviting a colleague.
	node::AccountPoster session_post;
	// Where the person saves a file, asked in the system's Save dialog (empty
	// when they cancel); and a file opened with the system's default
	// application. For the recovery codes (D-146).
	std::function<std::optional<std::string>(const std::string &default_name)> choose_save_path;
	std::function<bool(const std::string &path)> open_document;
	// This computer's user's store for a seat's password, kept only when the
	// member chooses so (D-243, Platform/SeatPasswordStore): which store it is
	// (empty where there is none), and keeping, reading and forgetting one
	// entry by its target. The page never reads what it keeps.
	std::function<std::optional<std::string>()> seat_password_store;
	std::function<bool(const std::string &target, const std::string &user, const std::string &password)> save_seat_password;
	std::function<std::optional<std::string>(const std::string &target)> read_seat_password;
	std::function<bool(const std::string &target)> forget_seat_password;
	// A problem report (account.reportProblem) and a work log
	// (account.shareWorkLog), posted with this computer's sign-in, whose
	// headers stay native: the address, the content type and the bytes. And the
	// version this build is (docs/VERSION) and the system it runs on.
	std::function<platform::HttpResponse(const std::string &url, const std::string &content_type, const std::string &body)> problem_post;
	std::string app_version;
	std::string platform_name;
	// This computer's device key (Platform/DeviceKey): its public half, and a
	// signature with it, which a work log carries inside its seal (D-257).
	std::function<std::string()> device_public_key;
	std::function<std::string(const std::string &message)> sign_with_device;
};

// account.reportProblem {title, category?, acceptedConsent?, description,
// technical, pictures: [{name, data}]} -> {number}: a problem report, made a
// card of the board Legal Machines keeps for this version's line (the owner,
// 2026-11-05; D-257), with this computer's sign-in, at most three screenshots
// of 10 MB. Legal Machines takes it from a design partner's seat, or from a
// Debug build.
nlohmann::json report_problem(const nlohmann::json &params, const AccountServices &services);

// account.workLogPreview {matterId, chatId} -> what a design partner's member
// sees before sending a chat's work log (D-257): its calls kept here, how many
// were protected or failed, when, how much, and whether it can be sent.
nlohmann::json work_log_preview(const nlohmann::json &params, const AccountServices &services);

// account.shareWorkLog {matterId, chatId, chatTitle, note, messages: [{author,
// at, text}], acceptedConsent?} -> {sent, sha256, recipients, bytes}: the chat's
// work log sealed on this computer to Legal Machines' keys and sent with this
// computer's sign-in (Node/WorkLogShare, D-257). The Diagnostics Consent the
// member ticked is kept with Legal Machines first, and nothing is sent unless
// it was.
nlohmann::json share_work_log(const nlohmann::json &params, const AccountServices &services);

// account.signIn {organisationId, seatId, password} -> {status, challengeId?,
// maskedEmail?, account}. On "signed_in" the account is saved (identifiers and
// display names only) and the protection model's download is started.
nlohmann::json sign_in(const nlohmann::json &params, const AccountServices &services);

// account.verifyTwoFactor {challengeId, code, organisationId, seatId, password,
// email?} -> {status: "signed_in", account}. The password is the one the person typed
// for this sign-in, still in the sign-in sheet; it is used for the download
// session and not kept. With {savedPassword: true} in place of the password,
// the one this computer kept for the seat whose sign-in made the challenge
// (account.signInWithEmail with savedPassword), held here for that challenge
// only and never said to the page (D-243).
nlohmann::json verify_two_factor(const nlohmann::json &params, const AccountServices &services);

// account.lookup {email} -> {exists}: whether Legal Machines knows an
// organisation by this email (D-133).
nlohmann::json lookup(const nlohmann::json &params, const AccountServices &services);

// account.signInWithEmail {email, seat?, password} -> {status, challengeId?,
// maskedEmail?, organisationId?, seatId?, account}: account.signIn by the
// firm's one email and a seat's id (D-204); with no seat, the firm's
// administrator seat. The answer names the organisation and seat the password
// opened, which account.verifyTwoFactor and account.resendEmailCode then take.
// A colleague's seat is signed in at once, with no second step. The firm's
// email is kept here (AccountResume::organisation_email).
// With {savedPassword: true} in place of the password, the seat signs in with
// the password this computer keeps for it (D-243), which the page never sees;
// when Legal Machines refuses it, or none is kept, it is forgotten and the
// answer is {status: "saved_password_refused", account}.
nlohmann::json sign_in_with_email(const nlohmann::json &params, const AccountServices &services);

// A seat's password kept on this computer at the member's choice (D-243),
// named by the firm's email and the seat as the sign-in names it (none for the
// administrator's). No call ever answers with the password.
// account.seatPasswordStore {} -> {store: "windows" | null}: where it can be
// kept here; null where it cannot.
// account.savedSeatPassword {email, seat?} -> {saved}: whether one is kept.
// account.saveSeatPassword {email, seat?, password} -> {saved}: kept only for
// the seat that signed in on this computer in the last ten minutes, and only
// the very password it signed in with; otherwise nothing is kept.
// account.forgetSeatPassword {email, seat?} -> {forgotten}.
nlohmann::json seat_password_store(const nlohmann::json &params, const AccountServices &services);
nlohmann::json saved_seat_password(const nlohmann::json &params, const AccountServices &services);
nlohmann::json save_seat_password(const nlohmann::json &params, const AccountServices &services);
nlohmann::json forget_seat_password(const nlohmann::json &params, const AccountServices &services);

// account.register {legalEntityName, country, registrationNumber, email,
// password, agreements, productUpdates?} -> {organisationId, seatId,
// maskedEmail}: a new organisation, and a code to its email. Nothing is kept
// here: the organisation signs in once its email is confirmed and Legal
// Machines has approved it (D-030).
nlohmann::json register_organisation(const nlohmann::json &params, const AccountServices &services);

// account.create {legalEntityName, country, registrationNumber, email,
// agreements, productUpdates?} -> {organisationId, seatId, maskedEmail}: a new
// organisation from its details and agreements alone, no password (D-146);
// a code goes to its email.
nlohmann::json create_organisation(const nlohmann::json &params, const AccountServices &services);

// account.confirmEmail {organisationId, code} -> {state, setup?}: state
// "awaiting_approval" or "active"; setup, for a new organisation (D-146),
// what its setup sets first ("password" or "two_factor"). The token that opens
// the setup is kept here, in memory, and never said to the page.
nlohmann::json confirm_email(const nlohmann::json &params, const AccountServices &services);

// A new organisation's setup (D-146), opened by the token kept here:
// account.setPassword {organisationId, password} -> {setup: "two_factor"};
// account.startTwoFactor {organisationId} -> {secret, uri};
// account.confirmTwoFactor {organisationId, code} -> {state, recoveryCodes}:
// two-factor on, the setup ended, the recovery codes kept here for saving or
// printing while the app runs.
nlohmann::json set_password(const nlohmann::json &params, const AccountServices &services);
nlohmann::json start_two_factor(const nlohmann::json &params, const AccountServices &services);
nlohmann::json confirm_two_factor(const nlohmann::json &params, const AccountServices &services);

// account.saveRecoveryCodes {organisationId} -> {saved}: the codes written to a
// text file where the person chooses in the Save dialog; false when they
// cancel.
nlohmann::json save_recovery_codes(const nlohmann::json &params, const AccountServices &services);

// account.printRecoveryCodes {organisationId} -> {printed}: the codes as a page
// to print, its print dialog opened in the system's browser. The page is kept
// for the person alone and deleted a few minutes later. (The window's own
// printing needs the platform layer's print support, which no system has
// yet.)
nlohmann::json print_recovery_codes(const nlohmann::json &params, const AccountServices &services);

// account.resendEmailCode {organisationId} -> {sent: true}.
nlohmann::json resend_email_code(const nlohmann::json &params, const AccountServices &services);

// account.approvalState {organisationId} -> {approved}: whether Legal Machines
// approved the organisation that waits for it (D-030).
nlohmann::json approval_state(const nlohmann::json &params, const AccountServices &services);

// The firm's seats (D-204), with this computer's session, which must be the
// firm's administrator's. Each seat is a login of the firm's with no person in
// it: its id, drawn by Legal Machines from a random set, and a password the
// administrator sets and hands to the colleague with the firm's email.
// account.listSeats -> {seats: [{seatId, role, createdAt}], organisationEmail};
// account.createSeat {seatId?, password} -> {seatId, organisationEmail}: the
// seat id the page drew and showed, from Legal Machines' own random set;
// account.setSeatPassword {seatId, password} -> {changed}: signs that seat's
// computers out; account.removeSeat {seatId} -> {removed}.
nlohmann::json list_seats(const nlohmann::json &params, const AccountServices &services);
nlohmann::json create_seat(const nlohmann::json &params, const AccountServices &services);
nlohmann::json set_seat_password(const nlohmann::json &params, const AccountServices &services);
nlohmann::json remove_seat(const nlohmann::json &params, const AccountServices &services);
// A seat's sign-in security (D-215): account.resetSeatTwoFactor {seatId} ->
// {reset} (the administrator's); account.ownSeat {} -> {seatId, role,
// twoFactor}; account.startOwnTwoFactor {} -> {secret, uri};
// account.confirmOwnTwoFactor {code} -> {recoveryCodes};
// account.changeOwnPassword {currentPassword, newPassword, code?} -> {changed};
// two-step changed as every service lets it be (D-218), each first confirmed
// by the password and a code the app shows or a recovery code:
// account.replaceOwnTwoFactor {password, code} -> {secret, uri} for a new
// app, account.confirmReplacedTwoFactor {code} -> {recoveryCodes} once the new
// app's code is right, account.renewOwnRecoveryCodes {password, code} ->
// {recoveryCodes};
// with no session, account.requestRecovery {email} -> {sent} and
// account.resetPassword {email, code, twoStepCode?, newPassword} -> {changed}.
nlohmann::json reset_seat_two_factor(const nlohmann::json &params, const AccountServices &services);
nlohmann::json own_seat(const nlohmann::json &params, const AccountServices &services);
nlohmann::json start_own_two_factor(const nlohmann::json &params, const AccountServices &services);
nlohmann::json confirm_own_two_factor(const nlohmann::json &params, const AccountServices &services);
nlohmann::json replace_own_two_factor(const nlohmann::json &params, const AccountServices &services);
nlohmann::json confirm_replaced_two_factor(const nlohmann::json &params, const AccountServices &services);
nlohmann::json renew_own_recovery_codes(const nlohmann::json &params, const AccountServices &services);
nlohmann::json change_own_password(const nlohmann::json &params, const AccountServices &services);
nlohmann::json request_recovery(const nlohmann::json &params, const AccountServices &services);
nlohmann::json reset_password(const nlohmann::json &params, const AccountServices &services);

// account.getState -> AccountState: who is signed in, never a token.
nlohmann::json get_state(const nlohmann::json &params, const AccountServices &services);

// account.signOut -> AccountState. Ends this computer's session at Legal
// Machines (D-026), then forgets the account and the download session here
// whatever the answer; an installed protection model stays installed and keeps
// working.
nlohmann::json sign_out(const nlohmann::json &params, const AccountServices &services);

} // namespace singularity::bridge::account_bridge
