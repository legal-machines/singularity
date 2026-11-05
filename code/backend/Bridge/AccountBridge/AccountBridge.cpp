#include "Bridge/AccountBridge/AccountBridge.hpp"
#include "Node/WorkLog/WorkLog.hpp"
#include "Node/WorkLogShare/WorkLogShare.hpp"
#include "Organisation/FirmRole/FirmRole.hpp"
#include "Persistence/PersistenceCoordinator/PersistenceCoordinator.hpp"
#include "Platform/AppSettings/AppSettings.hpp"
#include "Platform/PrivateFile/PrivateFile.hpp"
#include "Platform/SeatPasswordStore/SeatPasswordStore.hpp"
#include "Platform/Sha256/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace singularity::bridge::account_bridge {
namespace {

using Json = nlohmann::json;

std::string required_text(const Json &params, const char *key) {
	if (params.is_object() == false || params.contains(key) == false || params.at(key).is_string() == false) {
		throw std::invalid_argument(std::string("Expected ") + key + " as text");
	}
	return params.at(key).get<std::string>();
}

Json optional_text_json(const std::optional<std::string> &value) {
	return value.has_value() ? Json(*value) : Json(nullptr);
}

// A new organisation's setup on this computer (D-146): the token its
// confirmed email or a sign-in gave, and the recovery codes it was given, by
// organisation. In memory only, never said to the page, gone with the app.
struct Setup {
	std::string token;
	std::vector<std::string> recovery_codes;
};
std::mutex &setup_mutex() {
	static std::mutex mutex;
	return mutex;
}
std::map<std::string, Setup> &setups() {
	static std::map<std::string, Setup> held;
	return held;
}
void keep_setup_token(const std::string &organisation_id, const std::string &token) {
	const std::lock_guard<std::mutex> lock(setup_mutex());
	setups()[organisation_id] = Setup{token, {}};
}
void forget_setups() {
	const std::lock_guard<std::mutex> lock(setup_mutex());
	setups().clear();
}
std::string setup_token_of(const std::string &organisation_id) {
	const std::lock_guard<std::mutex> lock(setup_mutex());
	const auto found = setups().find(organisation_id);
	return found == setups().end() ? std::string() : found->second.token;
}
std::vector<std::string> recovery_codes_of(const std::string &organisation_id) {
	const std::lock_guard<std::mutex> lock(setup_mutex());
	const auto found = setups().find(organisation_id);
	if (found == setups().end() || found->second.recovery_codes.empty()) {
		throw node::AccountFailure(node::AccountFailureKind::InvalidInput, "The recovery codes are no longer here. Sign in to make new ones.");
	}
	return found->second.recovery_codes;
}

// The recovery codes as a person keeps them: what they are, the codes, and
// when they were made.
std::string recovery_text(const std::vector<std::string> &codes) {
	const std::time_t now = std::time(nullptr);
	std::tm parts{};
#ifdef _WIN32
	localtime_s(&parts, &now);
#else
	localtime_r(&now, &parts);
#endif
	char date[32];
	std::strftime(date, sizeof date, "%Y-%m-%d", &parts);
	std::string text = "Legal Machines recovery codes\n\n"
		"If you lose your authenticator app, each code signs you in to Singularity once.\n"
		"Keep them somewhere safe.\n\n";
	for (const auto &code : codes) text += code + "\n";
	text += std::string("\nMade on ") + date + "\n";
	return text;
}

// Whether the seat signed in finished onboarding on this computer before (D-250).
bool is_onboarded_here(const platform::AppSettings &settings) {
	if (settings.account.has_value() == false) return false;
	const std::string seat = platform::onboarded_seat_key(settings.account->organisation_id, settings.account->seat_id);
	return std::find(settings.onboarded_seats.begin(), settings.onboarded_seats.end(), seat) != settings.onboarded_seats.end();
}

Json account_state_json(const platform::AppSettings &settings, const AccountServices &services) {
	bool hasSession = false;
	if (services.has_session) {
		try {
			hasSession = services.has_session();
		} catch (...) {
			hasSession = false;
		}
	}
	if (settings.account.has_value() == false) {
		return Json{{"signedIn", false},
		            {"organisationId", nullptr},
		            {"seatId", nullptr},
		            {"role", nullptr},
		            {"organisationEmail", nullptr},
		            {"displayName", nullptr},
		            {"organisationName", nullptr},
		            {"hasDownloadSession", hasSession}};
	}
	const platform::AccountResume &account = *settings.account;
	return Json{{"signedIn", true},
	            {"organisationId", account.organisation_id},
	            {"seatId", account.seat_id},
	            {"role", organisation::is_administrator() ? "administrator" : "member"},
	            {"organisationEmail", optional_text_json(account.organisation_email)},
	            {"displayName", optional_text_json(account.display_name)},
	            {"organisationName", optional_text_json(account.organisation_name)},
	            {"hasDownloadSession", hasSession}};
}

// Keeps who signed in on this computer: identifiers and the names the account
// gives, never the password. Names the service did not send this time are kept
// from the last sign-in to the same organisation.
platform::AppSettings save_account(const std::string &organisation_id, const std::string &seat_id,
                                   const node::SignInAnswer *answer,
                                   const std::optional<std::string> &organisation_email = std::nullopt) {

	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	platform::AppSettings settings = platform::load_app_settings();
	platform::AccountResume resume;
	resume.organisation_id = organisation_id;
	resume.seat_id = seat_id;
	if (settings.account.has_value() == true && settings.account->organisation_id == organisation_id) {
		resume.contact_email_masked = settings.account->contact_email_masked;
		resume.contact_email_verified = settings.account->contact_email_verified;
		resume.organisation_name = settings.account->organisation_name;
		resume.organisation_email = settings.account->organisation_email;
		if (settings.account->seat_id == seat_id) resume.display_name = settings.account->display_name;
	}
	if (organisation_email.has_value() == true) resume.organisation_email = organisation_email;
	if (answer != nullptr) {
		if (answer->masked_email.has_value() == true) {
			resume.contact_email_masked = answer->masked_email;
			resume.contact_email_verified = answer->is_contact_verified;
		}
		if (answer->display_name.has_value() == true) resume.display_name = answer->display_name;
		if (answer->organisation_name.has_value() == true) resume.organisation_name = answer->organisation_name;
	}
	settings.account = resume;
	// A seat that finished onboarding on this computer goes straight back to
	// work here (D-250): its onboarding stands complete again.
	if (is_onboarded_here(settings) == true) settings.onboarding_completed = true;
	platform::save_app_settings(settings);
	forget_setups();
	return settings;

}

// The download of the protection model begins as soon as the account is kept.
// Not being able to start it is not a failed sign-in: the model's own status
// says what is missing and offers the way on.
void start_authorization(const AccountServices &services, const std::string &password,
		const std::string &challenge_id) {
	if (!services.start_authorization) return;
	try {
		services.start_authorization(password, challenge_id);
	} catch (const std::exception &) {
	}
}

// The seat's role as Legal Machines said it signing in (D-204); without it, as
// before: the role recorded for the same seat stands, any other is a member.
void record_seat_role(const std::string &seat_id, const node::SignInAnswer &answer) {
	if (answer.role == std::optional<std::string>("admin")) {
		organisation::record_role(seat_id, organisation::FirmRole::Administrator);
	} else if (answer.role == std::optional<std::string>("member")) {
		organisation::record_role(seat_id, organisation::FirmRole::Member);
	} else {
		organisation::record_signed_in(seat_id);
	}
}

// The firm's email the page sends along, as the member typed it to sign in or
// register (D-204), or nothing.
std::optional<std::string> organisation_email_of(const nlohmann::json &params) {
	if (params.contains("email") == false || params.at("email").is_string() == false) return std::nullopt;
	try {
		return node::normalized_email(params.at("email").get<std::string>());
	} catch (const node::AccountFailure &) {
		return std::nullopt;
	}
}

// A seat's role as the page says it: the firm's administrator, or a colleague.
std::string seat_role_words(const std::string &role) {
	return role == "admin" ? "administrator" : "member";
}

// Signed in; onboardedHere says whether this seat finished onboarding on this
// computer before, so the page goes straight back to work (D-250).
Json signed_in_json(const platform::AppSettings &settings, const AccountServices &services) {
	return Json{{"status", node::to_string(node::SignInStatus::SignedIn)},
	            {"account", account_state_json(settings, services)},
	            {"onboardedHere", is_onboarded_here(settings)}};
}

// A seat's own new recovery codes, kept in memory for Save as file and Print
// as a new firm's are, by its organisation; never written anywhere.
void keep_own_recovery_codes(const std::vector<std::string> &codes) {
	std::string organisation_id;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const platform::AppSettings settings = platform::load_app_settings();
		if (settings.account.has_value() == true) organisation_id = settings.account->organisation_id;
	}
	if (organisation_id.empty() == true) return;
	const std::lock_guard<std::mutex> lock(setup_mutex());
	setups()[organisation_id].recovery_codes = codes;
}

// A seat's password kept on this computer at the member's choice (D-243). Its
// entry is named by the firm's email and the seat as the sign-in names it
// (empty for the administrator's), under Singularity's own prefix, so no call
// reaches another entry. The page never reads it back: a sign-in asks for it
// with savedPassword, and it is used here.
constexpr auto kSavedPasswordWindow = std::chrono::minutes(10);

void wipe(std::string &secret) {
	secret.assign(secret.size(), '\0');
	secret.clear();
}

bool asks_for_saved_password(const Json &params) {
	return params.is_object() && params.contains("savedPassword") && params.at("savedPassword").is_boolean()
		&& params.at("savedPassword").get<bool>() == true;
}

// The seat as the sign-in names it: its id, or none for the administrator's.
std::string named_seat_of(const Json &params) {
	if (params.contains("seat") == false || params.at("seat").is_string() == false) return {};
	const std::string seat = params.at("seat").get<std::string>();
	if (seat.find_first_not_of(" \t\r\n") == std::string::npos) return {};
	return node::normalized_seat_id(seat);
}

std::string seat_password_target(const std::string &email, const std::string &seat) {
	return std::string(platform::seat_password_store::kTargetPrefix) + email + "/" + seat;
}

bool has_seat_password_store(const AccountServices &services) {
	return services.seat_password_store && services.save_seat_password && services.read_seat_password
		&& services.forget_seat_password && services.seat_password_store().has_value();
}

// Who signed in on this computer, and with which password, as a digest salted
// anew for each run: the one seat whose password may be kept, for ten
// minutes. In memory only; signing out forgets it.
struct SignedInSeat {
	std::string email;
	std::string seat;
	std::string password_digest;
	std::chrono::steady_clock::time_point at;
};

// A sign-in that asked for a second step: who it was, and the kept password
// it used (empty when the member typed theirs), for that challenge only.
struct PendingChallenge {
	SignedInSeat who;
	std::string saved_password;
};

std::mutex &memory_mutex() {
	static std::mutex mutex;
	return mutex;
}
std::optional<SignedInSeat> &last_signed_in_seat() {
	static std::optional<SignedInSeat> held;
	return held;
}
std::map<std::string, PendingChallenge> &pending_challenges() {
	static std::map<std::string, PendingChallenge> held;
	return held;
}

const std::string &digest_salt() {
	static const std::string salt = [] {
		std::random_device random;
		std::string value;
		for (int index = 0; index < 32; ++index) value.push_back(static_cast<char>(random() & 0xff));
		return value;
	}();
	return salt;
}

std::string digest_of(const std::string &password) {
	return platform::sha256_hex_digest(digest_salt() + password);
}

// Drops what has outlived its ten minutes, the kept passwords wiped. The
// memory lock is held.
void drop_stale_memory() {
	const auto now = std::chrono::steady_clock::now();
	auto &pending = pending_challenges();
	for (auto entry = pending.begin(); entry != pending.end();) {
		if (now - entry->second.who.at > kSavedPasswordWindow) {
			wipe(entry->second.saved_password);
			entry = pending.erase(entry);
		} else {
			++entry;
		}
	}
	auto &last = last_signed_in_seat();
	if (last.has_value() && now - last->at > kSavedPasswordWindow) last.reset();
}

void remember_signed_in(SignedInSeat who) {
	const std::lock_guard<std::mutex> lock(memory_mutex());
	who.at = std::chrono::steady_clock::now();
	last_signed_in_seat() = std::move(who);
}

void forget_signed_in_memory() {
	const std::lock_guard<std::mutex> lock(memory_mutex());
	for (auto &entry : pending_challenges()) wipe(entry.second.saved_password);
	pending_challenges().clear();
	last_signed_in_seat().reset();
}

// What answers a sign-in whose kept password could not be used: refused by
// Legal Machines, or none kept. It is forgotten; the page asks for the password.
Json saved_password_refused_json(const AccountServices &services) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	return Json{{"status", "saved_password_refused"}, {"account", account_state_json(platform::load_app_settings(), services)}};
}

} // namespace

nlohmann::json sign_in(const nlohmann::json &params, const AccountServices &services) {

	const std::string organisation_id = node::normalized_organisation_id(required_text(params, "organisationId"));
	const std::string seat_id = node::normalized_seat_id(required_text(params, "seatId"));
	const std::string password = required_text(params, "password");
	const node::SignInAnswer answer = node::sign_in(services.post, organisation_id, seat_id, password);

	if (answer.status == node::SignInStatus::TwoFactorRequired) {
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		return Json{{"status", node::to_string(answer.status)},
		            {"challengeId", answer.challenge_id},
		            {"method", answer.method},
		            {"account", account_state_json(platform::load_app_settings(), services)}};
	}
	if (answer.status == node::SignInStatus::AwaitingApproval) {
		// A new organisation Legal Machines has not approved yet (D-030): nothing
		// is kept and nothing is downloaded; the seat signs in again once it is.
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		return Json{{"status", node::to_string(answer.status)},
		            {"maskedEmail", optional_text_json(answer.masked_email)},
		            {"account", account_state_json(platform::load_app_settings(), services)}};
	}
	if (answer.status == node::SignInStatus::EmailVerificationRequired) {
		// The seat is right, but its organisation is not in use until its email
		// is confirmed: nothing is kept and nothing is downloaded yet.
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		return Json{{"status", node::to_string(answer.status)},
		            {"maskedEmail", optional_text_json(answer.masked_email)},
		            {"account", account_state_json(platform::load_app_settings(), services)}};
	}

	organisation::firm_role();
	const platform::AppSettings settings = save_account(organisation_id, seat_id, &answer);
	record_seat_role(seat_id, answer);
	start_authorization(services, password, answer.challenge_id);
	return signed_in_json(settings, services);

}

nlohmann::json verify_two_factor(const nlohmann::json &params, const AccountServices &services) {

	const std::string organisation_id = node::normalized_organisation_id(required_text(params, "organisationId"));
	const std::string seat_id = node::normalized_seat_id(required_text(params, "seatId"));
	const std::string challenge_id = required_text(params, "challengeId");
	// The password the member typed, or the one this computer keeps, held here
	// for the challenge its own sign-in made (D-243).
	std::string password;
	if (asks_for_saved_password(params) == true) {
		if (params.contains("password") == true && params.at("password").is_null() == false) {
			throw node::AccountFailure(node::AccountFailureKind::InvalidInput, "Give the password or ask for the saved one, not both.");
		}
		const std::lock_guard<std::mutex> lock(memory_mutex());
		drop_stale_memory();
		const auto found = pending_challenges().find(challenge_id);
		if (found != pending_challenges().end()) password = found->second.saved_password;
	} else {
		password = required_text(params, "password");
	}
	if (password.empty() == true) {
		throw node::AccountFailure(node::AccountFailureKind::InvalidInput, "Sign in again to get a new code.");
	}
	// The service names the organisation and the person once the code is right:
	// the app shows its own firm by that name, and a member signing in again
	// finds their own name (D-136).
	std::optional<node::SignInAnswer> answered;
	try {
		answered = node::verify_two_factor(services.post, challenge_id, required_text(params, "code"));
	} catch (...) {
		wipe(password);
		throw;
	}
	const node::SignInAnswer &named = *answered;

	// Who signed in: the seat whose sign-in made the challenge, as that sign-in
	// named it, or as this call does. Its kept password is let go.
	std::optional<SignedInSeat> who;
	{
		const std::lock_guard<std::mutex> lock(memory_mutex());
		const auto found = pending_challenges().find(challenge_id);
		if (found != pending_challenges().end()) {
			who = found->second.who;
			wipe(found->second.saved_password);
			pending_challenges().erase(found);
		}
	}
	if (who.has_value() == false) who = SignedInSeat{organisation_email_of(params).value_or(std::string()), seat_id, {}, {}};
	who->password_digest = digest_of(password);
	remember_signed_in(*who);

	// The download session is asked against this very check (the service
	// counts it for ten minutes), never against whichever code came last.
	organisation::firm_role();
	const platform::AppSettings settings = save_account(organisation_id, seat_id, &named, organisation_email_of(params));
	record_seat_role(seat_id, named);
	start_authorization(services, password, challenge_id);
	wipe(password);
	Json result = signed_in_json(settings, services);
	// Signed in by a recovery code (D-218): the page asks for a new
	// authenticator app and says how many codes are left.
	if (named.used_recovery_code == true) {
		result["usedRecoveryCode"] = true;
		if (named.recovery_codes_left.has_value() == true) result["recoveryCodesLeft"] = *named.recovery_codes_left;
	}
	return result;

}

nlohmann::json lookup(const nlohmann::json &params, const AccountServices &services) {
	const node::EmailLookup found = node::look_up_email(services.post, required_text(params, "email"));
	return Json{{"exists", found.exists}};
}

nlohmann::json list_seats(const nlohmann::json &, const AccountServices &services) {
	if (!services.session_post) throw node::AccountFailure(node::AccountFailureKind::WrongCredentials, "Sign in again to see the firm's seats.");
	const node::FirmSeats found = node::list_seats(services.session_post);
	Json seats = Json::array();
	for (const node::FirmSeat &seat : found.seats) {
		seats.push_back(Json{{"seatId", seat.seat_id}, {"role", seat_role_words(seat.role)}, {"createdAt", seat.created_at},
			{"twoFactor", seat.has_two_factor}});
	}
	// The firm's email as Legal Machines says it is kept here when this computer
	// signed in before it was kept (D-204).
	std::optional<std::string> email;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		platform::AppSettings settings = platform::load_app_settings();
		if (settings.account.has_value() == true) {
			if (found.organisation_email.has_value() && settings.account->organisation_email != found.organisation_email) {
				settings.account->organisation_email = found.organisation_email;
				platform::save_app_settings(settings);
			}
			email = settings.account->organisation_email;
		}
	}
	return Json{{"seats", seats}, {"organisationEmail", optional_text_json(email)}};
}

nlohmann::json create_seat(const nlohmann::json &params, const AccountServices &services) {
	if (!services.session_post) throw node::AccountFailure(node::AccountFailureKind::WrongCredentials, "Sign in again to add a seat.");
	// The login the page drew and showed (D-204), or none, for Legal Machines to draw.
	const std::string drawn = params.contains("seatId") && params.at("seatId").is_string() ? params.at("seatId").get<std::string>() : std::string();
	const std::string seat = node::create_seat(services.session_post, drawn, required_text(params, "password"));
	std::optional<std::string> email;
	{
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		const platform::AppSettings settings = platform::load_app_settings();
		if (settings.account.has_value() == true) email = settings.account->organisation_email;
	}
	return Json{{"seatId", seat}, {"organisationEmail", optional_text_json(email)}};
}

nlohmann::json set_seat_password(const nlohmann::json &params, const AccountServices &services) {
	if (!services.session_post) throw node::AccountFailure(node::AccountFailureKind::WrongCredentials, "Sign in again to change a seat's password.");
	node::set_seat_password(services.session_post, required_text(params, "seatId"), required_text(params, "password"));
	return Json{{"changed", true}};
}

nlohmann::json remove_seat(const nlohmann::json &params, const AccountServices &services) {
	if (!services.session_post) throw node::AccountFailure(node::AccountFailureKind::WrongCredentials, "Sign in again to remove a seat.");
	node::remove_seat(services.session_post, required_text(params, "seatId"));
	return Json{{"removed", true}};
}

nlohmann::json reset_seat_two_factor(const nlohmann::json &params, const AccountServices &services) {
	if (!services.session_post)
		throw node::AccountFailure(node::AccountFailureKind::WrongCredentials, "Sign in again to turn off a colleague's two-step sign-in.");
	node::reset_seat_two_factor(services.session_post, required_text(params, "seatId"));
	return Json{{"reset", true}};
}

nlohmann::json own_seat(const nlohmann::json &, const AccountServices &services) {
	if (!services.session_post) throw node::AccountFailure(node::AccountFailureKind::WrongCredentials, "Sign in again to see your seat.");
	const node::OwnSeat seat = node::own_seat(services.session_post);
	// A design partner's computer keeps its model calls for the work log its
	// members may send; any other keeps none, and erases what it kept. So does
	// a build without Legal Machines' keys for work logs, which could never
	// send what it kept (D-257).
	const bool keeps_work_logs = seat.is_design_partner && node::work_log_share::recipients_ready();
	node::work_log::set_kept(keeps_work_logs);
	Json result{{"seatId", seat.seat_id}, {"role", seat.role}, {"twoFactor", seat.has_two_factor}, {"designPartner", seat.is_design_partner},
		{"workLogs", keeps_work_logs},
		{"diagnosticsConsent", seat.diagnostics_consent.has_value() == true ? Json(*seat.diagnostics_consent) : Json(nullptr)}};
	if (seat.recovery_codes_left.has_value() == true) result["recoveryCodesLeft"] = *seat.recovery_codes_left;
	return result;
}

nlohmann::json start_own_two_factor(const nlohmann::json &, const AccountServices &services) {
	if (!services.session_post)
		throw node::AccountFailure(node::AccountFailureKind::WrongCredentials, "Sign in again to turn on two-step sign-in.");
	const node::OwnTwoFactorKey key = node::start_own_two_factor(services.session_post);
	return Json{{"secret", key.secret}, {"uri", key.uri}};
}

nlohmann::json confirm_own_two_factor(const nlohmann::json &params, const AccountServices &services) {
	if (!services.session_post)
		throw node::AccountFailure(node::AccountFailureKind::WrongCredentials, "Sign in again to turn on two-step sign-in.");
	const std::vector<std::string> codes = node::confirm_own_two_factor(services.session_post, required_text(params, "code"));
	keep_own_recovery_codes(codes);
	return Json{{"recoveryCodes", codes}};
}

nlohmann::json replace_own_two_factor(const nlohmann::json &params, const AccountServices &services) {
	if (!services.session_post)
		throw node::AccountFailure(node::AccountFailureKind::WrongCredentials, "Sign in again to change your authenticator app.");
	const node::OwnTwoFactorKey key =
		node::replace_own_two_factor(services.session_post, required_text(params, "password"), required_text(params, "code"));
	return Json{{"secret", key.secret}, {"uri", key.uri}};
}

nlohmann::json confirm_replaced_two_factor(const nlohmann::json &params, const AccountServices &services) {
	if (!services.session_post)
		throw node::AccountFailure(node::AccountFailureKind::WrongCredentials, "Sign in again to change your authenticator app.");
	const std::vector<std::string> codes = node::confirm_replaced_two_factor(services.session_post, required_text(params, "code"));
	keep_own_recovery_codes(codes);
	return Json{{"recoveryCodes", codes}};
}

nlohmann::json renew_own_recovery_codes(const nlohmann::json &params, const AccountServices &services) {
	if (!services.session_post)
		throw node::AccountFailure(node::AccountFailureKind::WrongCredentials, "Sign in again to make new recovery codes.");
	const std::vector<std::string> codes =
		node::renew_own_recovery_codes(services.session_post, required_text(params, "password"), required_text(params, "code"));
	keep_own_recovery_codes(codes);
	return Json{{"recoveryCodes", codes}};
}

nlohmann::json change_own_password(const nlohmann::json &params, const AccountServices &services) {
	if (!services.session_post) throw node::AccountFailure(node::AccountFailureKind::WrongCredentials, "Sign in again to change your password.");
	const std::string code = params.contains("code") && params.at("code").is_string() ? params.at("code").get<std::string>() : std::string();
	node::change_own_password(services.session_post, required_text(params, "currentPassword"), required_text(params, "newPassword"), code);
	return Json{{"changed", true}};
}

nlohmann::json request_recovery(const nlohmann::json &params, const AccountServices &services) {
	node::request_recovery(services.post, required_text(params, "email"));
	return Json{{"sent", true}};
}

nlohmann::json reset_password(const nlohmann::json &params, const AccountServices &services) {
	const std::string two_step = params.contains("twoStepCode") && params.at("twoStepCode").is_string()
		? params.at("twoStepCode").get<std::string>() : std::string();
	node::reset_password(services.post, required_text(params, "email"), required_text(params, "code"), two_step,
		required_text(params, "newPassword"));
	return Json{{"changed", true}};
}

namespace {

// Where a report goes: Legal Machines' board for its version's line (the mail
// server's /intake/singularity, which takes one from a design partner's seat,
// or from a Debug build, Legal Machines' own; D-257).
constexpr const char *kProblemReportUrl = "https://mail.legalmachines.org/intake/singularity";
constexpr std::size_t kMostProblemPictures = 3;
constexpr std::size_t kMostProblemPictureBytes = 10u * 1024 * 1024;

// Standard base64 (RFC 4648), padded or not; none where a character is not one.
std::optional<std::string> from_base64(const std::string &text) {
	std::string bytes;
	unsigned value = 0;
	int bits = 0;
	for (const char character : text) {
		int digit = -1;
		if (character >= 'A' && character <= 'Z') digit = character - 'A';
		else if (character >= 'a' && character <= 'z') digit = character - 'a' + 26;
		else if (character >= '0' && character <= '9') digit = character - '0' + 52;
		else if (character == '+') digit = 62;
		else if (character == '/') digit = 63;
		else if (character == '=') break;
		else return std::nullopt;
		value = (value << 6) | static_cast<unsigned>(digit);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			bytes.push_back(static_cast<char>((value >> bits) & 0xFF));
		}
	}
	return bytes;
}

// A name as one line of a form: no quotes, no line breaks.
std::string form_name(const std::string &name) {
	std::string out;
	for (const char character : name.substr(0, 200)) out += (character == '"' || character == '\r' || character == '\n' || character == '\\') ? '_' : character;
	return out.empty() == true ? std::string("screenshot") : out;
}

} // namespace

nlohmann::json report_problem(const nlohmann::json &params, const AccountServices &services) {
	if (!services.problem_post) throw node::AccountFailure(node::AccountFailureKind::ServiceUnavailable, "This build can't send a report.");
	const std::string title = required_text(params, "title");
	if (title.find_first_not_of(" \t\r\n") == std::string::npos)
		throw node::AccountFailure(node::AccountFailureKind::InvalidInput, "Give the report a title.");
	const auto text_of = [&params](const char *key) {
		return params.contains(key) && params.at(key).is_string() ? params.at(key).get<std::string>() : std::string();
	};
	// The Diagnostics Consent the member has just ticked for this seat (D-257):
	// kept by Legal Machines first, and nothing is sent unless it was.
	const std::string accepted = text_of("acceptedConsent");
	if (accepted.empty() == false) {
		if (!services.session_post) throw node::AccountFailure(node::AccountFailureKind::WrongCredentials, "Sign in again to send the report.");
		node::accept_diagnostics_consent(services.session_post, accepted);
	}
	std::random_device random;
	std::string boundary = "singularity-report-";
	for (int at = 0; at < 4; ++at) boundary += std::to_string(random());
	std::string body;
	const auto field = [&](const std::string &name, const std::string &value) {
		body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + value + "\r\n";
	};
	field("version", services.app_version);
	// A Debug build's report is Legal Machines' own; any other comes from a
	// design partner's seat alone, and the board labels it so (D-257).
#if defined(NDEBUG)
	field("build", "Release");
#else
	field("build", "Debug");
#endif
	field("title", title);
	// What it is about, by the id of the member's choice ("ux"), which the
	// board labels in its own words (D-257).
	const std::string category = text_of("category");
	if (category.empty() == false) {
		if (category.size() > 20 || std::all_of(category.begin(), category.end(), [](char c) { return c >= 'a' && c <= 'z'; }) == false)
			throw node::AccountFailure(node::AccountFailureKind::InvalidInput, "Choose what the report is about.");
		field("category", category);
	}
	field("description", text_of("description"));
	field("technical", text_of("technical"));
	if (params.contains("pictures") && params.at("pictures").is_array()) {
		const Json &pictures = params.at("pictures");
		if (pictures.size() > kMostProblemPictures)
			throw node::AccountFailure(node::AccountFailureKind::InvalidInput, "A report carries three screenshots at most.");
		for (const Json &picture : pictures) {
			if (picture.is_object() == false || picture.contains("data") == false || picture.at("data").is_string() == false)
				throw node::AccountFailure(node::AccountFailureKind::InvalidInput, "A screenshot could not be read.");
			const std::optional<std::string> bytes = from_base64(picture.at("data").get<std::string>());
			if (bytes.has_value() == false || bytes->empty() == true)
				throw node::AccountFailure(node::AccountFailureKind::InvalidInput, "A screenshot could not be read.");
			if (bytes->size() > kMostProblemPictureBytes)
				throw node::AccountFailure(node::AccountFailureKind::InvalidInput, "A screenshot can be 10 MB at most.");
			const std::string name = picture.contains("name") && picture.at("name").is_string() ? picture.at("name").get<std::string>() : std::string();
			body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"picture\"; filename=\"" + form_name(name)
				+ "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + *bytes + "\r\n";
		}
	}
	body += "--" + boundary + "--\r\n";
	const platform::HttpResponse response = services.problem_post(kProblemReportUrl, "multipart/form-data; boundary=" + boundary, body);
	const Json answer = Json::parse(response.body, nullptr, false);
	const std::string said = answer.is_object() && answer.contains("error") && answer.at("error").is_string() ? answer.at("error").get<std::string>()
		: std::string();
	if (response.status == 401)
		throw node::AccountFailure(node::AccountFailureKind::WrongCredentials, "Sign in to Singularity again, then send the report.");
	// Legal Machines takes reports from its design partners' seats alone.
	if (response.status == 403)
		throw node::AccountFailure(node::AccountFailureKind::Refused, said.empty() == true
			? std::string("Only Legal Machines' design partners send bug reports from Singularity.") : said);
	if (response.status == 429)
		throw node::AccountFailure(node::AccountFailureKind::TooManyAttempts, said.empty() == true ? std::string("Too many reports this hour. Send it a little later.") : said);
	if (response.status >= 400 && response.status < 500)
		throw node::AccountFailure(node::AccountFailureKind::InvalidInput, said.empty() == true ? std::string("The report was not taken.") : said);
	if (response.status < 200 || response.status >= 300 || answer.is_object() == false || answer.contains("number") == false
			|| answer.at("number").is_number_integer() == false)
		throw node::AccountFailure(node::AccountFailureKind::ServiceUnavailable, "The board isn't answering. Try again in a moment.");
	return Json{{"number", answer.at("number").get<long long>()}};
}

nlohmann::json work_log_preview(const nlohmann::json &params, const AccountServices &) {
	const node::work_log_share::Preview shown = node::work_log_share::preview(required_text(params, "matterId"), required_text(params, "chatId"));
	return Json{{"calls", shown.calls}, {"protectedCalls", shown.protected_calls}, {"unprotectedCalls", shown.unprotected_calls},
		{"failedCalls", shown.failed_calls}, {"from", shown.from}, {"to", shown.to}, {"bytes", shown.bytes}, {"canSend", shown.can_send},
		{"reason", shown.reason}};
}

nlohmann::json share_work_log(const nlohmann::json &params, const AccountServices &services) {
	if (!services.problem_post || !services.session_post)
		throw node::AccountFailure(node::AccountFailureKind::WrongCredentials, "Sign in to Singularity again, then send the work log.");
	const auto text_of = [&params](const char *key, std::size_t most) {
		const std::string text = params.contains(key) && params.at(key).is_string() ? params.at(key).get<std::string>() : std::string();
		if (text.size() > most) throw node::AccountFailure(node::AccountFailureKind::InvalidInput, "That is more than a work log carries.");
		return text;
	};
	node::work_log_share::Request request;
	request.matter_id = required_text(params, "matterId");
	request.chat_id = required_text(params, "chatId");
	request.chat_title = text_of("chatTitle", 300);
	request.note = text_of("note", 8000);
	// The conversation's messages as the member's page shows them: two
	// thousand at most, and 4 MB of words.
	if (params.contains("messages") && params.at("messages").is_array()) {
		std::size_t words = 0;
		for (const Json &message : params.at("messages")) {
			if (request.messages.size() >= 2000) break;
			if (message.is_object() == false) continue;
			node::work_log_share::Message kept;
			kept.author = message.contains("author") && message.at("author").is_string() ? message.at("author").get<std::string>() : std::string();
			kept.at = message.contains("at") && message.at("at").is_string() ? message.at("at").get<std::string>() : std::string();
			kept.text = message.contains("text") && message.at("text").is_string() ? message.at("text").get<std::string>() : std::string();
			words += kept.author.size() + kept.at.size() + kept.text.size();
			if (words > 4u * 1024 * 1024) break;
			request.messages.push_back(std::move(kept));
		}
	}
	// The Diagnostics Consent the member has just ticked for this seat (D-257):
	// kept with Legal Machines first, and nothing is sent unless it was.
	const std::string accepted = text_of("acceptedConsent", 20);
	if (accepted.empty() == false) node::accept_diagnostics_consent(services.session_post, accepted);
	const platform::AppSettings settings = platform::load_app_settings();
	request.facts.organisation = settings.account.has_value() == true ? settings.account->organisation_id : std::string();
	request.facts.seat = settings.account.has_value() == true ? settings.account->seat_id : std::string();
	request.facts.app_version = services.app_version;
#if defined(NDEBUG)
	request.facts.build = "Release";
#else
	request.facts.build = "Debug";
#endif
	request.facts.platform = services.platform_name;
	request.facts.device_public_key = services.device_public_key ? services.device_public_key() : std::string();
	const node::work_log_share::Poster post = [&services](const std::string &url, const std::string &content_type, const std::string &body) {
		const platform::HttpResponse response = services.problem_post(url, content_type, body);
		return std::make_pair(static_cast<long>(response.status), response.body);
	};
	try {
		const node::work_log_share::Sent sent = node::work_log_share::send(request, services.sign_with_device, post);
		return Json{{"sent", true}, {"sha256", sent.sha256}, {"recipients", sent.recipients}, {"bytes", sent.bytes}};
	} catch (const node::AccountFailure &) {
		throw;
	} catch (const std::runtime_error &error) {
		throw node::AccountFailure(node::AccountFailureKind::Refused, error.what());
	}
}

nlohmann::json sign_in_with_email(const nlohmann::json &params, const AccountServices &services) {

	const std::string email = required_text(params, "email");
	// The seat's id (D-204); a page before seats names none, the administrator's.
	const std::string seat = params.contains("seat") && params.at("seat").is_string() ? params.at("seat").get<std::string>() : std::string();
	// The password the member typed, or the one this computer keeps for the
	// seat (D-243), read here and never said to the page.
	const bool from_store = asks_for_saved_password(params);
	const std::optional<std::string> firm_email = organisation_email_of(params);
	const std::string named_seat = named_seat_of(params);
	std::string target;
	std::string password;
	if (from_store == true) {
		if (params.contains("password") == true && params.at("password").is_null() == false) {
			throw node::AccountFailure(node::AccountFailureKind::InvalidInput, "Give the password or ask for the saved one, not both.");
		}
		if (firm_email.has_value() == false || has_seat_password_store(services) == false) return saved_password_refused_json(services);
		target = seat_password_target(*firm_email, named_seat);
		std::optional<std::string> kept = services.read_seat_password(target);
		if (kept.has_value() == false || kept->empty() == true) return saved_password_refused_json(services);
		password = std::move(*kept);
	} else {
		password = required_text(params, "password");
	}
	std::optional<node::SignInAnswer> answered;
	try {
		answered = node::sign_in_with_email(services.post, email, seat, password);
	} catch (const node::AccountFailure &failure) {
		wipe(password);
		// A kept password Legal Machines refuses is forgotten, and said so.
		if (from_store == true && failure.getKind() == node::AccountFailureKind::WrongCredentials) {
			services.forget_seat_password(target);
			return saved_password_refused_json(services);
		}
		throw;
	} catch (...) {
		wipe(password);
		throw;
	}
	const node::SignInAnswer &answer = *answered;
	const auto with_seat = [&answer](Json result) {
		if (answer.organisation_id.has_value()) result["organisationId"] = *answer.organisation_id;
		if (answer.seat_id.has_value()) result["seatId"] = *answer.seat_id;
		return result;
	};
	const SignedInSeat who{firm_email.value_or(std::string()), named_seat, digest_of(password), {}};

	if (answer.status == node::SignInStatus::TwoFactorRequired) {
		// The code's check comes next: who this was, and the kept password, for
		// that challenge alone.
		{
			const std::lock_guard<std::mutex> lock(memory_mutex());
			drop_stale_memory();
			PendingChallenge pending{who, from_store ? password : std::string()};
			pending.who.at = std::chrono::steady_clock::now();
			pending_challenges()[answer.challenge_id] = std::move(pending);
		}
		wipe(password);
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		return with_seat(Json{{"status", node::to_string(answer.status)},
		                      {"challengeId", answer.challenge_id},
		                      {"method", answer.method},
		                      {"maskedEmail", optional_text_json(answer.masked_email)},
		                      {"account", account_state_json(platform::load_app_settings(), services)}});
	}
	// A new organisation whose setup is not finished (D-146): it goes on with
	// two-factor authentication; the token stays here.
	if (answer.status == node::SignInStatus::SetupRequired && answer.organisation_id.has_value()) {
		wipe(password);
		keep_setup_token(*answer.organisation_id, answer.setup_token);
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		return with_seat(Json{{"status", node::to_string(answer.status)},
		                      {"setup", answer.setup},
		                      {"maskedEmail", optional_text_json(answer.masked_email)},
		                      {"account", account_state_json(platform::load_app_settings(), services)}});
	}
	if (answer.status != node::SignInStatus::SignedIn) {
		wipe(password);
		// Waiting for approval, or for the email to be confirmed: nothing is
		// kept and nothing is downloaded (D-030).
		const persistence::PersistenceLock lock(persistence::persistence_mutex());
		return with_seat(Json{{"status", node::to_string(answer.status)},
		                      {"maskedEmail", optional_text_json(answer.masked_email)},
		                      {"account", account_state_json(platform::load_app_settings(), services)}});
	}

	organisation::firm_role();
	const platform::AppSettings settings =
		save_account(*answer.organisation_id, *answer.seat_id, &answer, node::normalized_email(email));
	record_seat_role(*answer.seat_id, answer);
	remember_signed_in(who);
	// A colleague's seat's sign-in stands checked (D-204): this computer's
	// session is asked against it.
	start_authorization(services, password, answer.challenge_id);
	wipe(password);
	Json result = signed_in_json(settings, services);
	// A colleague's seat with no authenticator yet sets one up before it works (D-215).
	if (answer.needs_two_factor_setup == true) result["twoFactorSetup"] = "required";
	return with_seat(result);

}

nlohmann::json seat_password_store(const nlohmann::json &, const AccountServices &services) {
	if (!services.seat_password_store) return Json{{"store", nullptr}};
	return Json{{"store", optional_text_json(services.seat_password_store())}};
}

nlohmann::json saved_seat_password(const nlohmann::json &params, const AccountServices &services) {
	const std::string target = seat_password_target(node::normalized_email(required_text(params, "email")), named_seat_of(params));
	if (has_seat_password_store(services) == false) return Json{{"saved", false}};
	std::optional<std::string> kept = services.read_seat_password(target);
	const bool saved = kept.has_value() == true && kept->empty() == false;
	if (kept.has_value() == true) wipe(*kept);
	return Json{{"saved", saved}};
}

nlohmann::json save_seat_password(const nlohmann::json &params, const AccountServices &services) {
	const std::string email = node::normalized_email(required_text(params, "email"));
	const std::string seat = named_seat_of(params);
	std::string password = required_text(params, "password");
	// Only the seat that signed in here in the last ten minutes, and only the
	// very password it signed in with.
	bool may_keep = false;
	{
		const std::lock_guard<std::mutex> lock(memory_mutex());
		drop_stale_memory();
		const std::optional<SignedInSeat> &last = last_signed_in_seat();
		may_keep = password.empty() == false && last.has_value() == true && last->email == email && last->seat == seat
			&& last->password_digest == digest_of(password);
	}
	bool saved = false;
	if (may_keep == true && has_seat_password_store(services) == true) {
		saved = services.save_seat_password(seat_password_target(email, seat), seat.empty() ? email : seat, password);
	}
	wipe(password);
	return Json{{"saved", saved}};
}

nlohmann::json forget_seat_password(const nlohmann::json &params, const AccountServices &services) {
	const std::string target = seat_password_target(node::normalized_email(required_text(params, "email")), named_seat_of(params));
	const bool forgotten = has_seat_password_store(services) == true && services.forget_seat_password(target) == true;
	return Json{{"forgotten", forgotten}};
}

nlohmann::json register_organisation(const nlohmann::json &params, const AccountServices &services) {

	node::OrganisationRegistration registration;
	registration.legal_entity_name = required_text(params, "legalEntityName");
	registration.country = required_text(params, "country");
	registration.registration_number = required_text(params, "registrationNumber");
	registration.contact_email = required_text(params, "email");
	registration.password = required_text(params, "password");
	if (params.contains("agreements") == false || params.at("agreements").is_object() == false) {
		throw std::invalid_argument("Expected agreements as an object");
	}
	const Json &agreements = params.at("agreements");
	const auto accepted = [&agreements](const char *key) {
		return agreements.contains(key) && agreements.at(key).is_boolean() && agreements.at(key).get<bool>();
	};
	registration.accepts_licence_and_terms = accepted("licenceAndTerms");
	registration.acknowledges_privacy_notice = accepted("privacyNotice");
	registration.consents_to_personal_data = accepted("personalData");
	registration.confirms_authority = accepted("authority");
	registration.confirms_shared_mailbox = accepted("sharedMailbox");
	registration.wants_product_updates = params.value("productUpdates", false);

	const node::RegistrationAnswer answer = node::register_organisation(services.post, registration);
	return Json{{"organisationId", answer.organisation_id},
	            {"seatId", answer.seat_id},
	            {"maskedEmail", optional_text_json(answer.masked_email)}};

}

nlohmann::json create_organisation(const nlohmann::json &params, const AccountServices &services) {

	node::OrganisationRegistration registration;
	registration.legal_entity_name = required_text(params, "legalEntityName");
	registration.country = required_text(params, "country");
	registration.registration_number = required_text(params, "registrationNumber");
	registration.contact_email = required_text(params, "email");
	if (params.contains("agreements") == false || params.at("agreements").is_object() == false) {
		throw std::invalid_argument("Expected agreements as an object");
	}
	const Json &agreements = params.at("agreements");
	const auto accepted = [&agreements](const char *key) {
		return agreements.contains(key) && agreements.at(key).is_boolean() && agreements.at(key).get<bool>();
	};
	registration.accepts_licence_and_terms = accepted("licenceAndTerms");
	registration.acknowledges_privacy_notice = accepted("privacyNotice");
	registration.consents_to_personal_data = accepted("personalData");
	registration.confirms_authority = accepted("authority");
	registration.confirms_shared_mailbox = accepted("sharedMailbox");
	registration.wants_product_updates = params.value("productUpdates", false);

	const node::RegistrationAnswer answer = node::create_organisation(services.post, registration);
	return Json{{"organisationId", answer.organisation_id},
	            {"seatId", answer.seat_id},
	            {"maskedEmail", optional_text_json(answer.masked_email)}};

}

nlohmann::json confirm_email(const nlohmann::json &params, const AccountServices &services) {
	const std::string organisation_id = node::normalized_organisation_id(required_text(params, "organisationId"));
	const node::ContactConfirmed confirmed =
		node::confirm_contact(services.post, organisation_id, required_text(params, "code"));
	Json result{{"state", node::to_string(confirmed.state)}};
	if (confirmed.setup.empty() == false) {
		keep_setup_token(organisation_id, confirmed.setup_token);
		result["setup"] = confirmed.setup;
	}
	return result;
}

nlohmann::json set_password(const nlohmann::json &params, const AccountServices &services) {
	const std::string organisation_id = node::normalized_organisation_id(required_text(params, "organisationId"));
	node::set_initial_password(services.post, organisation_id, setup_token_of(organisation_id),
	                           required_text(params, "password"));
	return Json{{"setup", "two_factor"}};
}

nlohmann::json start_two_factor(const nlohmann::json &params, const AccountServices &services) {
	const std::string organisation_id = node::normalized_organisation_id(required_text(params, "organisationId"));
	const node::TwoFactorKey key = node::start_two_factor(services.post, organisation_id, setup_token_of(organisation_id));
	return Json{{"secret", key.secret}, {"uri", key.uri}};
}

nlohmann::json confirm_two_factor(const nlohmann::json &params, const AccountServices &services) {
	const std::string organisation_id = node::normalized_organisation_id(required_text(params, "organisationId"));
	const node::TwoFactorOn on = node::confirm_two_factor(services.post, organisation_id, setup_token_of(organisation_id),
	                                                      required_text(params, "code"));
	Json result{{"state", node::to_string(on.state)}, {"recoveryCodes", on.recovery_codes}};
	// Active at the end of its setup, the administrator is signed in here at
	// once, with the password just set and the sign-in the service gives: the
	// code is not asked again (D-152). The password is used for this one call
	// and never kept. Without the service's sign-in, the page signs in with a
	// code as before.
	const std::string seat_id = params.value("seatId", std::string());
	const std::string password = params.value("password", std::string());
	// The firm's own registration is complete on this computer: its seat is
	// the administrator here (D-184), approved yet or not.
	if (seat_id.empty() == false) organisation::record_registered(node::normalized_seat_id(seat_id));
	if (on.state == node::ContactConfirmation::Active && on.challenge_id.empty() == false && seat_id.empty() == false
	    && password.empty() == false) {
		node::SignInAnswer named;
		named.organisation_name = on.organisation_name;
		const platform::AppSettings settings =
			save_account(organisation_id, node::normalized_seat_id(seat_id), &named, organisation_email_of(params));
		start_authorization(services, password, on.challenge_id);
		result["signedIn"] = signed_in_json(settings, services);
	}
	{
		// After the account is kept, which forgets every setup: the codes stay
		// for Save as file and Print until onboarding goes on.
		const std::lock_guard<std::mutex> lock(setup_mutex());
		Setup &setup = setups()[organisation_id];
		setup.token.clear();
		setup.recovery_codes = on.recovery_codes;
	}
	return result;
}

nlohmann::json save_recovery_codes(const nlohmann::json &params, const AccountServices &services) {
	const std::string organisation_id = node::normalized_organisation_id(required_text(params, "organisationId"));
	const std::vector<std::string> codes = recovery_codes_of(organisation_id);
	if (!services.choose_save_path) return Json{{"saved", false}};
	const std::optional<std::string> path = services.choose_save_path("Legal Machines recovery codes.txt");
	if (path.has_value() == false || path->empty() == true) return Json{{"saved", false}};
	const auto destination = std::filesystem::u8path(*path);
	std::ofstream file(destination, std::ios::binary | std::ios::trunc);
	if (!file || !platform::restrict_to_owner(destination)) {
		throw std::runtime_error("The recovery codes could not be saved privately. Choose another place.");
	}
	file << recovery_text(codes);
	file.close();
	if (!file) throw std::runtime_error("The recovery codes could not be written there. Choose another place.");
	return Json{{"saved", true}};
}

nlohmann::json print_recovery_codes(const nlohmann::json &params, const AccountServices &services) {
	const std::string organisation_id = node::normalized_organisation_id(required_text(params, "organisationId"));
	const std::vector<std::string> codes = recovery_codes_of(organisation_id);
	if (!services.open_document) return Json{{"printed", false}};

	// A page of the person's own, in a folder only they can read, deleted a
	// few minutes after the browser has it.
	std::error_code error;
	const std::filesystem::path folder = std::filesystem::temp_directory_path(error) / "Singularity recovery codes";
	if (error) throw std::runtime_error("The page to print could not be made. Save the codes as a file instead.");
	std::filesystem::create_directories(folder, error);
	if (error || !platform::restrict_to_owner(folder)) {
		throw std::runtime_error("The page to print could not be made privately. Save the codes as a file instead.");
	}
	std::random_device random;
	const std::filesystem::path page = folder / ("codes-" + std::to_string(random()) + ".html");
	std::string rows;
	for (const auto &code : codes) rows += "<li>" + code + "</li>";
	{
		std::ofstream file(page, std::ios::binary | std::ios::trunc);
		if (!file || !platform::restrict_to_owner(page)) {
			throw std::runtime_error("The page to print could not be made privately. Save the codes as a file instead.");
		}
		file << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>Legal Machines recovery codes</title>"
			"<style>body{font:15px/1.5 -apple-system,'Segoe UI',Roboto,sans-serif;color:#10100d;margin:48px}"
			"h1{font-size:24px;font-weight:600;margin:0 0 8px}p{margin:0 0 24px;color:#4a4a46}"
			"ol{columns:2;max-width:360px;padding:0;margin:0;list-style:none;font:17px/2 ui-monospace,Menlo,Consolas,monospace}"
			"</style></head><body><h1>Legal Machines recovery codes</h1>"
			"<p>If you lose your authenticator app, each code signs you in to Singularity once. Keep this page somewhere safe.</p>"
			"<ol>" << rows << "</ol><script>addEventListener('load',()=>setTimeout(()=>print(),300))</script></body></html>";
		file.close();
		if (!file) {
			std::filesystem::remove(page, error);
			throw std::runtime_error("The page to print could not be written. Save the codes as a file instead.");
		}
	}
	const bool opened = services.open_document(page.string());
	std::thread([page] {
		std::this_thread::sleep_for(std::chrono::minutes(3));
		std::error_code ignored;
		std::filesystem::remove(page, ignored);
	}).detach();
	return Json{{"printed", opened}};
}

nlohmann::json approval_state(const nlohmann::json &params, const AccountServices &services) {
	return Json{{"approved", node::organisation_is_approved(services.post, required_text(params, "organisationId"))}};
}

nlohmann::json resend_email_code(const nlohmann::json &params, const AccountServices &services) {
	node::resend_contact_code(services.post, required_text(params, "organisationId"));
	return Json{{"sent", true}};
}

nlohmann::json get_state(const nlohmann::json &, const AccountServices &services) {
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	return account_state_json(platform::load_app_settings(), services);
}

nlohmann::json sign_out(const nlohmann::json &, const AccountServices &services) {

	if (services.forget_session) services.forget_session();
	forget_setups();
	// Who signed in here, and a sign-in's held password, go with the session: no
	// password is kept for a seat after it signed out (D-243). One the member
	// chose to keep stays kept.
	forget_signed_in_memory();
	// The role of the seat signing out is settled first, so it signs in again
	// to the role it had (D-184).
	organisation::firm_role();
	const persistence::PersistenceLock lock(persistence::persistence_mutex());
	platform::AppSettings settings = platform::load_app_settings();
	// A seat that finished onboarding before the seats were recorded here is
	// recorded as it leaves, so it does not go through onboarding once more
	// when it signs in again (D-250).
	if (settings.account.has_value() == true && settings.onboarding_completed == true && is_onboarded_here(settings) == false) {
		settings.onboarded_seats.push_back(platform::onboarded_seat_key(settings.account->organisation_id, settings.account->seat_id));
	}
	settings.account.reset();
	// Signed out, this computer opens at the entry again: the next member signs
	// in there, and one who has been here goes straight back to work (D-136,
	// D-250: the seats that finished onboarding here stay listed).
	settings.onboarding_completed = false;
	// No file keeps the account afterwards, not even the settings' recovery
	// copy (the owner, 2026-11-05).
	platform::save_app_settings_leaving_no_copy(settings);
	return account_state_json(settings, services);

}

} // namespace singularity::bridge::account_bridge
