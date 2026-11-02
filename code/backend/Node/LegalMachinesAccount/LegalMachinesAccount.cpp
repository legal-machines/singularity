#include "Node/LegalMachinesAccount/LegalMachinesAccount.hpp"
#include "Node/LegalMachinesConnection/LegalMachinesConnection.hpp"
#include "Platform/Events/Events.hpp"
#include "Platform/Trace/Trace.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <set>
#include <string>

namespace singularity::node {
namespace {

// A new organisation waits for Legal Machines' approval (D-030): said as what
// it is, never as a wrong password.
const char *kAwaitingApproval = "Legal Machines has not approved your organisation yet, so this computer cannot "
	"download the protection model. Legal Machines reviews each new organisation and writes to its contact address "
	"once it is approved; then sign in again.";

using Json = nlohmann::json;

const char *kUnreachable = "Singularity could not reach Legal Machines. Check the internet connection, then try again.";
const char *kServiceUnavailable =
	"Legal Machines is not answering sign-ins right now. Your work in Singularity is unaffected. Try again in a few minutes.";
const char *kWrongCredentials = "The Organisation ID, Seat ID or password is not right. Check them and try again.";
const char *kTooManyAttempts =
	"Legal Machines paused signing in to this seat after several tries. Wait a few minutes, then try again.";
const char *kWrongEmailOrPassword = "The email or password is not right. Check them and try again.";
const char *kInvalidEmail = "Enter the organisation's email, like legal@example.com.";
const char *kAlreadyRegistered = "An organisation with these details is registered already. Sign in with its email.";
const char *kWrongContactCode = "That code was not accepted. Check it and try again, or ask for a new one.";

std::string trimmed_lower(const std::string &value) {
	const std::size_t first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return "";
	const std::size_t last = value.find_last_not_of(" \t\r\n");
	std::string result = value.substr(first, last - first + 1);
	for (char &character : result) {
		character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	}
	return result;
}

bool is_identifier_character(char character) {
	return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-';
}

// One request to the service. A request that got no answer at all - offline,
// a refused connection, a timeout - is said the same way: the words of the
// transport are the operating system's, in its own language, and never reach
// the person.
platform::HttpResponse post_or_fail(const AccountPoster &post, const std::string &path, const Json &body) {
	try {
		return post(std::string(kLegalMachinesApiBase) + path, body.dump());
	} catch (const std::exception &) {
		throw AccountFailure(AccountFailureKind::Offline, kUnreachable);
	}
}

Json parsed_or_fail(const std::string &body) {
	try {
		const Json parsed = Json::parse(body);
		if (parsed.is_object() == false) throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
		return parsed;
	} catch (const Json::exception &) {
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
}

std::optional<std::string> text_at(const Json &object, const char *key) {
	if (object.is_object() == false || object.contains(key) == false || object.at(key).is_string() == false) {
		return std::nullopt;
	}
	const std::string value = object.at(key).get<std::string>();
	if (value.empty() == true) return std::nullopt;
	return value;
}

bool flag_at(const Json &object, const char *key) {
	return object.is_object() && object.contains(key) && object.at(key).is_boolean() && object.at(key).get<bool>() == true;
}

void record_outcome(const char *name, const std::string &outcome) {
	platform::trace::record("account", name, {{"outcome", outcome}});
}

} // namespace

bool is_two_step_setup_refusal(const platform::HttpResponse &response) {
	if (response.status != 403) return false;
	const Json refusal = Json::parse(response.body, nullptr, false);
	return refusal.is_object() == true && text_at(refusal, "twoFactorSetup") == std::optional<std::string>("required");
}

void tell_two_step_setup_required() {
	static std::atomic<long long> last_told{-1};
	const long long now =
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	long long told = last_told.load();
	if (told >= 0 && now - told < 10) return;
	if (last_told.compare_exchange_strong(told, now) == false) return;
	platform::events::publish("account.twoStepSetupRequired", Json::object());
}

const char *to_string(SignInStatus status) {
	switch (status) {
		case SignInStatus::SignedIn: return "signed_in";
		case SignInStatus::TwoFactorRequired: return "two_factor_required";
		case SignInStatus::EmailVerificationRequired: return "email_verification_required";
		case SignInStatus::AwaitingApproval: return "awaiting_approval";
		case SignInStatus::SetupRequired: return "setup_required";
	}
	return "signed_in";
}

// The service's own identifier rules (organisation: 1-32 of a-z, 0-9 and "-",
// not starting or ending with "-"; seat: a letter, then 2-63 of a-z, 0-9, "-").
std::string normalized_organisation_id(const std::string &organisation_id) {

	const std::string value = trimmed_lower(organisation_id);
	bool isValid = value.empty() == false && value.size() <= 32 && value.front() != '-' && value.back() != '-';
	for (const char character : value) {
		if (is_identifier_character(character) == false) isValid = false;
	}
	if (isValid == false) {
		throw AccountFailure(AccountFailureKind::InvalidInput,
			"Check the Organisation ID. It is written in small letters, digits and dashes, like example-trading.");
	}
	return value;

}

std::string normalized_seat_id(const std::string &seat_id) {

	const std::string value = trimmed_lower(seat_id);
	bool isValid = value.size() >= 3 && value.size() <= 64 && value.front() >= 'a' && value.front() <= 'z';
	for (const char character : value) {
		if (is_identifier_character(character) == false) isValid = false;
	}
	if (isValid == false) {
		throw AccountFailure(AccountFailureKind::InvalidInput,
			"Check the Seat ID. It begins with a letter and is written in small letters, digits and dashes, like ulpian-7k4.");
	}
	return value;

}

namespace {

// What a sign-in's answer says, by Organisation and Seat IDs or by email alike.
// `wrong` is the sentence for a wrong password, in the words the person used.
SignInAnswer answer_of(const platform::HttpResponse &response, const char *wrong) {

	if (response.status == 400 || response.status == 401 || response.status == 403 || response.status == 404) {
		record_outcome("sign_in", "wrong_credentials");
		throw AccountFailure(AccountFailureKind::WrongCredentials, wrong);
	}
	if (response.status == 423 || response.status == 429) {
		record_outcome("sign_in", "too_many_attempts");
		throw AccountFailure(AccountFailureKind::TooManyAttempts, kTooManyAttempts);
	}
	if (response.status < 200 || response.status >= 300) {
		record_outcome("sign_in", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}

	/* - - - - - - */

	const Json payload = parsed_or_fail(response.body);
	SignInAnswer answer;
	answer.organisation_id = text_at(payload, "organisationId");
	answer.seat_id = text_at(payload, "seatId");
	// A new organisation that Legal Machines has not approved yet: the password
	// was right, and nothing more happens until it is approved (D-030).
	if (flag_at(payload, "awaitingApproval") == true) {
		const Json contact = payload.contains("contact") ? payload.at("contact") : Json::object();
		answer.masked_email = text_at(contact, "maskedEmail");
		answer.status = SignInStatus::AwaitingApproval;
		record_outcome("sign_in", to_string(answer.status));
		return answer;
	}
	// A new organisation whose setup is not finished (D-146): it goes on with
	// two-factor authentication, opened by the token the answer carries.
	if (flag_at(payload, "requiresSetup") == true) {
		const std::optional<std::string> token = text_at(payload, "setupToken");
		if (token.has_value() == false) {
			record_outcome("sign_in", "service_unavailable");
			throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
		}
		const Json contact = payload.contains("contact") ? payload.at("contact") : Json::object();
		answer.masked_email = text_at(contact, "maskedEmail");
		answer.status = SignInStatus::SetupRequired;
		answer.setup = text_at(payload, "setup").value_or("two_factor");
		answer.setup_token = *token;
		record_outcome("sign_in", to_string(answer.status));
		return answer;
	}
	if (flag_at(payload, "requiresTwoFactor") == true) {
		const std::optional<std::string> challenge = text_at(payload, "challengeId");
		if (challenge.has_value() == false) {
			record_outcome("sign_in", "service_unavailable");
			throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
		}
		const Json contact = payload.contains("contact") ? payload.at("contact") : Json::object();
		answer.masked_email = text_at(contact, "maskedEmail");
		answer.status = SignInStatus::TwoFactorRequired;
		answer.challenge_id = *challenge;
		answer.method = text_at(payload, "method") == std::optional<std::string>("authenticator") ? "authenticator" : "email";
		record_outcome("sign_in", to_string(answer.status));
		return answer;
	}

	const Json contact = payload.contains("contact") ? payload.at("contact") : Json::object();
	answer.masked_email = text_at(contact, "maskedEmail");
	answer.is_contact_verified = flag_at(contact, "verified");
	answer.display_name = text_at(payload, "displayName");
	answer.organisation_name = text_at(payload, "organisationName");
	// A colleague's seat signs in with no second step (D-204): the sign-in
	// stands checked, and this computer's session is asked against it.
	answer.challenge_id = text_at(payload, "challengeId").value_or("");
	answer.role = text_at(payload, "role");
	answer.needs_two_factor_setup = text_at(payload, "twoFactorSetup") == std::optional<std::string>("required");
	if (answer.organisation_name.has_value() == false && payload.contains("organisation")) {
		answer.organisation_name = text_at(payload.at("organisation"), "name");
	}
	answer.status = flag_at(payload, "requiresEmailVerification") == true
		? SignInStatus::EmailVerificationRequired
		: SignInStatus::SignedIn;
	record_outcome("sign_in", to_string(answer.status));
	return answer;

}

// The service's own words for a registration it could not take, when they are
// a short sentence; ours otherwise. They say what to correct, never a secret.
std::string refusal_words(const std::string &body, const char *fallback) {
	const Json refusal = Json::parse(body, nullptr, false);
	const std::optional<std::string> words = text_at(refusal, "error");
	if (words.has_value() == false || words->size() > 200 || words->find('\n') != std::string::npos) return fallback;
	return *words;
}

} // namespace

std::string normalized_email(const std::string &email) {

	const std::string value = trimmed_lower(email);
	const std::size_t at = value.find('@');
	bool isValid = value.size() >= 6 && value.size() <= 254 && at != std::string::npos && at > 0
		&& value.find('@', at + 1) == std::string::npos;
	if (isValid == true) {
		const std::string domain = value.substr(at + 1);
		const std::size_t dot = domain.rfind('.');
		// A domain with a name and a top level of two letters at least.
		isValid = dot != std::string::npos && dot > 0 && dot + 3 <= domain.size();
	}
	for (const char character : value) {
		if (std::isspace(static_cast<unsigned char>(character)) != 0 || character == ',' || character == ';') isValid = false;
	}
	if (isValid == false) throw AccountFailure(AccountFailureKind::InvalidInput, kInvalidEmail);
	return value;

}

SignInAnswer sign_in(const AccountPoster &post,
                     const std::string &organisation_id,
                     const std::string &seat_id,
                     const std::string &password) {

	const std::string organisation = normalized_organisation_id(organisation_id);
	const std::string seat = normalized_seat_id(seat_id);
	if (password.empty() == true) throw AccountFailure(AccountFailureKind::InvalidInput, "Enter the password of this seat.");

	platform::HttpResponse response;
	try {
		response = post_or_fail(post, "/auth/login",
			Json{{"organisationId", organisation}, {"seatId", seat}, {"password", password}});
	} catch (const AccountFailure &failure) {
		record_outcome("sign_in", failure.code());
		throw;
	}
	return answer_of(response, kWrongCredentials);

}

bool organisation_is_approved(const AccountPoster &post, const std::string &organisation_id) {

	const std::string organisation = normalized_organisation_id(organisation_id);
	if (organisation.empty() == true) throw AccountFailure(AccountFailureKind::InvalidInput, "Sign in again.");
	platform::HttpResponse response;
	try {
		response = post_or_fail(post, "/organisations/contact-status", Json{{"organisationId", organisation}});
	} catch (const AccountFailure &failure) {
		record_outcome("approval_state", failure.code());
		throw;
	}
	if (response.status < 200 || response.status >= 300) {
		record_outcome("approval_state", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	const Json payload = parsed_or_fail(response.body);
	const bool approved = flag_at(payload, "verified");
	record_outcome("approval_state", approved ? "approved" : "waiting");
	return approved;

}

EmailLookup look_up_email(const AccountPoster &post, const std::string &email) {

	const std::string address = normalized_email(email);
	platform::HttpResponse response;
	try {
		response = post_or_fail(post, "/organisations/lookup", Json{{"email", address}});
	} catch (const AccountFailure &failure) {
		record_outcome("lookup", failure.code());
		throw;
	}
	if (response.status == 400) {
		record_outcome("lookup", "invalid_input");
		throw AccountFailure(AccountFailureKind::InvalidInput, kInvalidEmail);
	}
	if (response.status == 429) {
		record_outcome("lookup", "too_many_attempts");
		throw AccountFailure(AccountFailureKind::TooManyAttempts,
			"Legal Machines paused these checks after many tries. Wait a few minutes, then try again.");
	}
	if (response.status < 200 || response.status >= 300) {
		record_outcome("lookup", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	const Json payload = parsed_or_fail(response.body);
	if (payload.contains("exists") == false || payload.at("exists").is_boolean() == false) {
		record_outcome("lookup", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	EmailLookup found;
	found.exists = payload.at("exists").get<bool>();
	record_outcome("lookup", found.exists ? "known" : "unknown");
	return found;

}

namespace {

// A seat request the service did not take, in its own words where they are a
// short sentence (D-204): signing in again, only the administrator, too many
// today, or what to correct.
[[noreturn]] void refuse_seat_request(const platform::HttpResponse &response, const char *outcome, const char *again,
		const char *fallback) {
	if (response.status == 401) {
		record_outcome(outcome, "wrong_credentials");
		throw AccountFailure(AccountFailureKind::WrongCredentials, again);
	}
	if (response.status == 429) {
		record_outcome(outcome, "too_many_attempts");
		throw AccountFailure(AccountFailureKind::TooManyAttempts,
			refusal_words(response.body, "The firm added many seats today. Try again tomorrow."));
	}
	if (response.status >= 400 && response.status < 500) {
		record_outcome(outcome, "invalid_input");
		throw AccountFailure(AccountFailureKind::InvalidInput, refusal_words(response.body, fallback));
	}
	record_outcome(outcome, "service_unavailable");
	throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
}

platform::HttpResponse post_seat_request(const AccountPoster &session_post, const char *path, const Json &body,
		const char *outcome) {
	platform::HttpResponse response;
	try {
		response = post_or_fail(session_post, path, body);
	} catch (const AccountFailure &failure) {
		record_outcome(outcome, failure.code());
		throw;
	}
	// Every door of the seat's session but setting up two steps is shut until
	// it has an authenticator app (D-215): said as that, wherever it is met.
	if (is_two_step_setup_refusal(response) == true) {
		record_outcome(outcome, "two_step_setup_required");
		tell_two_step_setup_required();
		throw AccountFailure(AccountFailureKind::TwoStepSetupRequired, kTwoStepSetupFirst);
	}
	return response;
}

void check_seat_password(const std::string &password) {
	if (password.size() < 12) throw AccountFailure(AccountFailureKind::InvalidInput, "Choose a password of at least 12 characters.");
	if (password.size() > 200) throw AccountFailure(AccountFailureKind::InvalidInput, "Choose a password of at most 200 characters.");
}

} // namespace

FirmSeats list_seats(const AccountPoster &session_post) {

	const platform::HttpResponse response = post_seat_request(session_post, "/organisations/seats", Json::object(), "list_seats");
	if (response.status < 200 || response.status >= 300) {
		refuse_seat_request(response, "list_seats", "Sign in again to see the firm's seats.", "The firm's seats could not be read.");
	}
	const Json payload = parsed_or_fail(response.body);
	FirmSeats found;
	if (payload.contains("seats") == true && payload.at("seats").is_array() == true) {
		for (const Json &entry : payload.at("seats")) {
			FirmSeat seat;
			seat.seat_id = text_at(entry, "seatId").value_or("");
			seat.role = text_at(entry, "role").value_or("member");
			seat.created_at = text_at(entry, "createdAt").value_or("");
			seat.has_two_factor = flag_at(entry, "twoFactor");
			if (seat.seat_id.empty() == false) found.seats.push_back(std::move(seat));
		}
	}
	if (const std::optional<std::string> email = text_at(payload, "organisationEmail"); email.has_value() && email->empty() == false) {
		try {
			found.organisation_email = normalized_email(*email);
		} catch (const AccountFailure &) {
		}
	}
	record_outcome("list_seats", "listed");
	return found;

}

std::string create_seat(const AccountPoster &session_post, const std::string &seat_id, const std::string &password) {

	check_seat_password(password);
	Json request{{"password", password}};
	if (trimmed_lower(seat_id).empty() == false) request["seatId"] = normalized_seat_id(seat_id);
	const platform::HttpResponse response =
		post_seat_request(session_post, "/organisations/seats/create", request, "create_seat");
	if (response.status < 200 || response.status >= 300) {
		refuse_seat_request(response, "create_seat", "Sign in again to add a seat.", "The seat could not be made. Try again.");
	}
	const std::optional<std::string> seat = text_at(parsed_or_fail(response.body), "seatId");
	if (seat.has_value() == false || seat->empty() == true) {
		record_outcome("create_seat", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	record_outcome("create_seat", "created");
	return *seat;

}

void set_seat_password(const AccountPoster &session_post, const std::string &seat_id, const std::string &password) {

	check_seat_password(password);
	const platform::HttpResponse response = post_seat_request(session_post, "/organisations/seats/password",
		Json{{"seatId", normalized_seat_id(seat_id)}, {"password", password}}, "set_seat_password");
	if (response.status < 200 || response.status >= 300) {
		refuse_seat_request(response, "set_seat_password", "Sign in again to change a seat's password.",
			"The password could not be changed. Try again.");
	}
	record_outcome("set_seat_password", "changed");

}

void remove_seat(const AccountPoster &session_post, const std::string &seat_id) {

	const platform::HttpResponse response = post_seat_request(session_post, "/organisations/seats/remove",
		Json{{"seatId", normalized_seat_id(seat_id)}}, "remove_seat");
	if (response.status < 200 || response.status >= 300) {
		refuse_seat_request(response, "remove_seat", "Sign in again to remove a seat.", "The seat could not be removed. Try again.");
	}
	record_outcome("remove_seat", "removed");

}

void reset_seat_two_factor(const AccountPoster &session_post, const std::string &seat_id) {

	const platform::HttpResponse response = post_seat_request(session_post, "/organisations/seats/two-factor/reset",
		Json{{"seatId", normalized_seat_id(seat_id)}}, "reset_seat_two_factor");
	if (response.status < 200 || response.status >= 300) {
		refuse_seat_request(response, "reset_seat_two_factor", "Sign in again to turn off a colleague's two-step sign-in.",
			"Two-step sign-in could not be turned off. Try again.");
	}
	record_outcome("reset_seat_two_factor", "reset");

}

namespace {

// A refusal of the seat's own security call, in the service's words: a wrong
// password or code is said as such, never as a session to sign in again for.
[[noreturn]] void refuse_own_request(const platform::HttpResponse &response, const char *outcome, const char *fallback) {
	if (response.status == 429) {
		record_outcome(outcome, "too_many_attempts");
		throw AccountFailure(AccountFailureKind::TooManyAttempts, refusal_words(response.body, "Too many tries. Wait a few minutes."));
	}
	if (response.status == 401) {
		const std::string words = refusal_words(response.body, fallback);
		record_outcome(outcome, "wrong_code");
		throw AccountFailure(words.rfind("Sign in again", 0) == 0 ? AccountFailureKind::WrongCredentials : AccountFailureKind::WrongCode,
			words);
	}
	if (response.status >= 400 && response.status < 500) {
		record_outcome(outcome, "invalid_input");
		throw AccountFailure(AccountFailureKind::InvalidInput, refusal_words(response.body, fallback));
	}
	record_outcome(outcome, "service_unavailable");
	throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
}

// A second step's code as the service reads it: six digits, or a recovery
// code's eight letters and digits (D-146) in capitals, its hyphen and spaces
// aside; none when it is neither.
std::optional<std::string> second_step_code(const std::string &code) {
	std::string digits;
	bool letters = false;
	for (const char character : code) {
		if (character == '-' || std::isspace(static_cast<unsigned char>(character)) != 0) continue;
		if (std::isalnum(static_cast<unsigned char>(character)) == 0) return std::nullopt;
		if (std::isdigit(static_cast<unsigned char>(character)) == 0) letters = true;
		digits += static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
	}
	const bool six_digits = digits.size() == 6 && letters == false;
	if (six_digits == false && digits.size() != 8) return std::nullopt;
	return digits;
}

// A count an answer carries (D-218), or none.
std::optional<int> count_at(const Json &payload, const char *key) {
	if (payload.contains(key) == false || payload.at(key).is_number_integer() == false) return std::nullopt;
	const long long count = payload.at(key).get<long long>();
	if (count < 0 || count > 1000) return std::nullopt;
	return static_cast<int>(count);
}

// The recovery codes an answer carries, each XXXX-XXXX in capitals and
// digits. They are shown in the app and written to a printable page, so only
// a complete answer is taken: none, a broken one or one twice is the service
// failing, never a code silently dropped or two steps left without codes.
std::vector<std::string> recovery_codes_in(const Json &payload, const char *operation) {
	const auto refused = [operation] {
		record_outcome(operation, "service_unavailable");
		return AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	};
	if (!payload.contains("recoveryCodes") || !payload.at("recoveryCodes").is_array() || payload.at("recoveryCodes").empty()) throw refused();
	const auto is_code_character = [](char character) {
		return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9');
	};
	std::vector<std::string> codes;
	std::set<std::string> seen;
	for (const Json &recovery : payload.at("recoveryCodes")) {
		if (!recovery.is_string()) throw refused();
		const std::string value = recovery.get<std::string>();
		const bool valid = value.size() == 9 && value[4] == '-' && std::all_of(value.begin(), value.begin() + 4, is_code_character)
			&& std::all_of(value.begin() + 5, value.end(), is_code_character);
		if (!valid || !seen.insert(value).second) throw refused();
		codes.push_back(value);
	}
	return codes;
}

// The password and the code that confirm it is the seat's own person (D-218),
// the code as the service reads it; refused before anything is sent.
std::string confirmed_by(const std::string &password, const std::string &code) {
	if (password.empty() == true) throw AccountFailure(AccountFailureKind::InvalidInput, "Enter your password.");
	const std::optional<std::string> sent = second_step_code(code);
	if (sent.has_value() == false)
		throw AccountFailure(AccountFailureKind::InvalidInput, "Enter the code your authenticator app shows, or a recovery code.");
	return *sent;
}

} // namespace

OwnSeat own_seat(const AccountPoster &session_post) {

	const platform::HttpResponse response = post_seat_request(session_post, "/organisations/seats/own", Json::object(), "own_seat");
	if (response.status < 200 || response.status >= 300) refuse_own_request(response, "own_seat", "Your seat could not be read.");
	const Json payload = parsed_or_fail(response.body);
	OwnSeat seat;
	seat.seat_id = text_at(payload, "seatId").value_or("");
	seat.role = text_at(payload, "role").value_or("member");
	seat.has_two_factor = flag_at(payload, "twoFactor");
	seat.recovery_codes_left = count_at(payload, "recoveryCodesLeft");
	record_outcome("own_seat", "read");
	return seat;

}

OwnTwoFactorKey replace_own_two_factor(const AccountPoster &session_post, const std::string &password, const std::string &code) {

	const std::string sent = confirmed_by(password, code);
	const platform::HttpResponse response = post_seat_request(session_post, "/organisations/seats/own/two-factor/replace",
		Json{{"password", password}, {"code", sent}}, "replace_own_two_factor");
	if (response.status < 200 || response.status >= 300)
		refuse_own_request(response, "replace_own_two_factor", "The new app could not be set up. Try again.");
	const Json payload = parsed_or_fail(response.body);
	OwnTwoFactorKey key;
	key.secret = text_at(payload, "secret").value_or("");
	key.uri = text_at(payload, "uri").value_or("");
	if (key.secret.empty() == true || key.uri.rfind("otpauth://", 0) != 0) {
		record_outcome("replace_own_two_factor", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	record_outcome("replace_own_two_factor", "started");
	return key;

}

std::vector<std::string> confirm_replaced_two_factor(const AccountPoster &session_post, const std::string &code) {

	const std::optional<std::string> sent = second_step_code(code);
	if (sent.has_value() == false || sent->size() != 6)
		throw AccountFailure(AccountFailureKind::InvalidInput, "Enter the six digits the new app shows.");
	const platform::HttpResponse response = post_seat_request(session_post, "/organisations/seats/own/two-factor/replace/confirm",
		Json{{"code", *sent}}, "confirm_replaced_two_factor");
	if (response.status < 200 || response.status >= 300)
		refuse_own_request(response, "confirm_replaced_two_factor", "That code doesn't match. Try the one the new app shows now.");
	const std::vector<std::string> codes = recovery_codes_in(parsed_or_fail(response.body), "confirm_replaced_two_factor");
	record_outcome("confirm_replaced_two_factor", "replaced");
	return codes;

}

std::vector<std::string> renew_own_recovery_codes(const AccountPoster &session_post, const std::string &password,
		const std::string &code) {

	const std::string sent = confirmed_by(password, code);
	const platform::HttpResponse response = post_seat_request(session_post, "/organisations/seats/own/two-factor/recovery-codes",
		Json{{"password", password}, {"code", sent}}, "renew_own_recovery_codes");
	if (response.status < 200 || response.status >= 300)
		refuse_own_request(response, "renew_own_recovery_codes", "New recovery codes could not be made. Try again.");
	const std::vector<std::string> codes = recovery_codes_in(parsed_or_fail(response.body), "renew_own_recovery_codes");
	record_outcome("renew_own_recovery_codes", "renewed");
	return codes;

}

OwnTwoFactorKey start_own_two_factor(const AccountPoster &session_post) {

	const platform::HttpResponse response =
		post_seat_request(session_post, "/organisations/seats/own/two-factor", Json::object(), "start_own_two_factor");
	if (response.status < 200 || response.status >= 300)
		refuse_own_request(response, "start_own_two_factor", "Two-step sign-in could not be set up. Try again.");
	const Json payload = parsed_or_fail(response.body);
	OwnTwoFactorKey key;
	key.secret = text_at(payload, "secret").value_or("");
	key.uri = text_at(payload, "uri").value_or("");
	if (key.secret.empty() == true || key.uri.rfind("otpauth://", 0) != 0) {
		record_outcome("start_own_two_factor", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	record_outcome("start_own_two_factor", "started");
	return key;

}

std::vector<std::string> confirm_own_two_factor(const AccountPoster &session_post, const std::string &code) {

	const platform::HttpResponse response =
		post_seat_request(session_post, "/organisations/seats/own/two-factor/confirm", Json{{"code", code}}, "confirm_own_two_factor");
	if (response.status < 200 || response.status >= 300)
		refuse_own_request(response, "confirm_own_two_factor", "That code doesn't match. Try the one the app shows now.");
	const std::vector<std::string> codes = recovery_codes_in(parsed_or_fail(response.body), "confirm_own_two_factor");
	record_outcome("confirm_own_two_factor", "on");
	return codes;

}

void change_own_password(const AccountPoster &session_post, const std::string &current, const std::string &wanted,
		const std::string &code) {

	check_seat_password(wanted);
	if (current.empty() == true) throw AccountFailure(AccountFailureKind::InvalidInput, "Enter your current password.");
	Json request{{"currentPassword", current}, {"newPassword", wanted}};
	if (code.empty() == false) request["code"] = code;
	const platform::HttpResponse response = post_seat_request(session_post, "/organisations/seats/own/password", request,
		"change_own_password");
	if (response.status < 200 || response.status >= 300)
		refuse_own_request(response, "change_own_password", "The password could not be changed. Try again.");
	record_outcome("change_own_password", "changed");

}

void request_recovery(const AccountPoster &post, const std::string &email) {

	const platform::HttpResponse response = post_or_fail(post, "/organisations/recover", Json{{"email", normalized_email(email)}});
	if (response.status < 200 || response.status >= 300)
		refuse_own_request(response, "request_recovery", "A code could not be sent. Try again.");
	record_outcome("request_recovery", "sent");

}

void reset_password(const AccountPoster &post, const std::string &email, const std::string &email_code,
		const std::string &two_step_code, const std::string &wanted) {

	if (wanted.size() < 12) throw AccountFailure(AccountFailureKind::InvalidInput, "Choose a password of at least 12 characters.");
	if (wanted.size() > 200) throw AccountFailure(AccountFailureKind::InvalidInput, "Choose a password of at most 200 characters.");
	Json request{{"email", normalized_email(email)}, {"code", email_code}, {"newPassword", wanted}};
	if (two_step_code.empty() == false) request["twoStepCode"] = two_step_code;
	const platform::HttpResponse response = post_or_fail(post, "/organisations/reset-password", request);
	if (response.status < 200 || response.status >= 300)
		refuse_own_request(response, "reset_password", "The password could not be changed. Try again.");
	record_outcome("reset_password", "changed");

}

namespace {

// What the firm's computers say of themselves goes quietly: a refusal is said
// in the service's words, a failure as the service being away.
Json computer_request(const AccountPoster &session_post, const char *path, const Json &body, const char *outcome) {
	const platform::HttpResponse response = post_seat_request(session_post, path, body, outcome);
	if (response.status < 200 || response.status >= 300) {
		refuse_seat_request(response, outcome, "Sign in again to reach the firm's computers.", "The firm's computers could not be reached.");
	}
	record_outcome(outcome, "done");
	return parsed_or_fail(response.body);
}

} // namespace

void announce_computer(const AccountPoster &session_post, const ListedComputer &self) {
	Json addresses = Json::array();
	for (const std::string &address : self.addresses) addresses.push_back(address);
	(void)computer_request(session_post, "/organisations/computers/announce",
		Json{{"deviceId", self.device_id}, {"deviceKey", self.device_key}, {"coordinationKey", self.coordination_key},
			{"signature", self.signature}, {"addresses", addresses}},
		"announce_computer");
}

std::vector<ListedComputer> list_computers(const AccountPoster &session_post) {
	const Json payload = computer_request(session_post, "/organisations/computers", Json::object(), "list_computers");
	std::vector<ListedComputer> computers;
	if (payload.contains("computers") == false || payload.at("computers").is_array() == false) return computers;
	for (const Json &entry : payload.at("computers")) {
		ListedComputer computer;
		computer.seat_id = text_at(entry, "seatId").value_or("");
		computer.role = text_at(entry, "role").value_or("");
		computer.device_id = text_at(entry, "deviceId").value_or("");
		computer.device_key = text_at(entry, "deviceKey").value_or("");
		computer.coordination_key = text_at(entry, "coordinationKey").value_or("");
		computer.signature = text_at(entry, "signature").value_or("");
		computer.announced_at = text_at(entry, "announcedAt").value_or("");
		if (entry.contains("addresses") && entry.at("addresses").is_array()) {
			for (const Json &address : entry.at("addresses"))
				if (address.is_string()) computer.addresses.push_back(address.get<std::string>());
		}
		if (computer.seat_id.empty() == false && computer.device_id.empty() == false) computers.push_back(std::move(computer));
	}
	return computers;
}

void send_pairing(const AccountPoster &session_post, const std::string &from_device, const std::string &to_device,
		const std::string &kind, const std::string &value) {
	(void)computer_request(session_post, "/organisations/computers/pairing/send",
		Json{{"fromDeviceId", from_device}, {"toDeviceId", to_device}, {"kind", kind}, {"value", value}}, "send_pairing");
}

std::vector<PairingMessage> pairing_inbox(const AccountPoster &session_post, const std::string &device_id) {
	const Json payload = computer_request(session_post, "/organisations/computers/pairing/inbox", Json{{"deviceId", device_id}}, "pairing_inbox");
	std::vector<PairingMessage> messages;
	if (payload.contains("messages") == false || payload.at("messages").is_array() == false) return messages;
	for (const Json &entry : payload.at("messages")) {
		PairingMessage message;
		message.from_device = text_at(entry, "fromDeviceId").value_or("");
		message.kind = text_at(entry, "kind").value_or("");
		message.value = text_at(entry, "value").value_or("");
		if (message.from_device.empty() == false) messages.push_back(std::move(message));
	}
	return messages;
}

bool organisation_exists(const AccountPoster &post, const std::string &email) {

	const std::string address = normalized_email(email);
	platform::HttpResponse response;
	try {
		response = post_or_fail(post, "/organisations/lookup", Json{{"email", address}});
	} catch (const AccountFailure &failure) {
		record_outcome("lookup", failure.code());
		throw;
	}
	if (response.status == 400) {
		record_outcome("lookup", "invalid_input");
		throw AccountFailure(AccountFailureKind::InvalidInput, kInvalidEmail);
	}
	if (response.status == 429) {
		record_outcome("lookup", "too_many_attempts");
		throw AccountFailure(AccountFailureKind::TooManyAttempts,
			"Legal Machines paused these checks after many tries. Wait a few minutes, then try again.");
	}
	if (response.status < 200 || response.status >= 300) {
		record_outcome("lookup", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	const Json payload = parsed_or_fail(response.body);
	if (payload.contains("exists") == false || payload.at("exists").is_boolean() == false) {
		record_outcome("lookup", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	const bool exists = payload.at("exists").get<bool>();
	record_outcome("lookup", exists ? "known" : "unknown");
	return exists;

}

SignInAnswer sign_in_with_email(const AccountPoster &post, const std::string &email, const std::string &seat,
		const std::string &password) {

	const std::string address = normalized_email(email);
	if (password.empty() == true) throw AccountFailure(AccountFailureKind::InvalidInput, "Enter the seat's password.");

	// The seat by its id (D-204); none opens the firm's administrator seat, as
	// before seats.
	Json request{{"email", address}, {"password", password}};
	if (trimmed_lower(seat).empty() == false) request["seat"] = normalized_seat_id(seat);
	platform::HttpResponse response;
	try {
		response = post_or_fail(post, "/auth/login", request);
	} catch (const AccountFailure &failure) {
		record_outcome("sign_in", failure.code());
		throw;
	}
	SignInAnswer answer = answer_of(response, kWrongEmailOrPassword);
	// Signed in by email, the service must say which seat the password opened:
	// everything after this - the code, the download session - is that seat's.
	const bool namesSeat = answer.organisation_id.has_value() && answer.seat_id.has_value();
	if (answer.status != SignInStatus::AwaitingApproval && namesSeat == false) {
		record_outcome("sign_in", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	if (namesSeat == true) {
		answer.organisation_id = normalized_organisation_id(*answer.organisation_id);
		answer.seat_id = normalized_seat_id(*answer.seat_id);
	}
	return answer;

}

RegistrationAnswer register_organisation(const AccountPoster &post, const OrganisationRegistration &registration) {

	const auto text = [](const std::string &value) {
		const std::size_t first = value.find_first_not_of(" \t\r\n");
		if (first == std::string::npos) return std::string();
		return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
	};
	const std::string name = text(registration.legal_entity_name);
	const std::string country = text(registration.country);
	const std::string number = text(registration.registration_number);
	if (name.empty() || country.empty() || number.empty()) {
		throw AccountFailure(AccountFailureKind::InvalidInput, "Enter the organisation's legal name, country and registration number.");
	}
	if (name.size() > 200 || country.size() > 120 || number.size() > 120) {
		throw AccountFailure(AccountFailureKind::InvalidInput, "Shorten the organisation's details.");
	}
	const std::string email = normalized_email(registration.contact_email);
	if (registration.password.size() < 8) {
		throw AccountFailure(AccountFailureKind::InvalidInput, "Choose a password of at least 8 characters.");
	}
	if (registration.password.size() > 200) {
		throw AccountFailure(AccountFailureKind::InvalidInput, "Choose a shorter password.");
	}
	if (!registration.accepts_licence_and_terms || !registration.acknowledges_privacy_notice ||
	    !registration.consents_to_personal_data || !registration.confirms_authority ||
	    !registration.confirms_shared_mailbox) {
		throw AccountFailure(AccountFailureKind::InvalidInput, "Accept each statement to register.");
	}

	/* - - - - - - */

	platform::HttpResponse started;
	try {
		started = post_or_fail(post, "/organisations/register-start",
			Json{{"legalEntityName", name}, {"country", country}, {"registrationNumber", number},
			     {"organisationContactEmail", email}});
	} catch (const AccountFailure &failure) {
		record_outcome("register", failure.code());
		throw;
	}
	if (started.status == 409) {
		record_outcome("register", "already_registered");
		throw AccountFailure(AccountFailureKind::AlreadyRegistered, kAlreadyRegistered);
	}
	if (started.status == 429) {
		record_outcome("register", "too_many_attempts");
		throw AccountFailure(AccountFailureKind::TooManyAttempts,
			"Legal Machines paused registrations from this network after many tries. Wait an hour, then try again.");
	}
	if (started.status == 400) {
		record_outcome("register", "invalid_input");
		throw AccountFailure(AccountFailureKind::InvalidInput,
			refusal_words(started.body, "Check the organisation's details."));
	}
	if (started.status < 200 || started.status >= 300) {
		record_outcome("register", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	const std::optional<std::string> named = text_at(parsed_or_fail(started.body), "organisationId");
	if (named.has_value() == false) {
		record_outcome("register", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	const std::string organisation = normalized_organisation_id(*named);

	/* - - - - - - */

	const Json agreements{{"softwareLicenceAccepted", registration.accepts_licence_and_terms},
	                      {"termsOfUseAccepted", registration.accepts_licence_and_terms},
	                      {"privacyNoticeAcknowledged", registration.acknowledges_privacy_notice},
	                      {"personalDataConsentAccepted", registration.consents_to_personal_data},
	                      {"authorityConfirmed", registration.confirms_authority},
	                      {"organisationEmailRepresentationAccepted", registration.confirms_shared_mailbox}};
	platform::HttpResponse completed;
	try {
		completed = post_or_fail(post, "/organisations/register-complete",
			Json{{"organisationId", organisation}, {"password", registration.password}, {"agreements", agreements},
			     {"productUpdatesOptIn", registration.wants_product_updates}});
	} catch (const AccountFailure &failure) {
		record_outcome("register", failure.code());
		throw;
	}
	if (completed.status == 409) {
		record_outcome("register", "already_registered");
		throw AccountFailure(AccountFailureKind::AlreadyRegistered, kAlreadyRegistered);
	}
	if (completed.status == 400 || completed.status == 404) {
		record_outcome("register", "invalid_input");
		throw AccountFailure(AccountFailureKind::InvalidInput,
			refusal_words(completed.body, "Registration did not go through. Start again."));
	}
	if (completed.status == 429) {
		record_outcome("register", "too_many_attempts");
		throw AccountFailure(AccountFailureKind::TooManyAttempts, kTooManyAttempts);
	}
	if (completed.status < 200 || completed.status >= 300) {
		record_outcome("register", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	const Json payload = parsed_or_fail(completed.body);
	const std::optional<std::string> seat = text_at(payload, "seatId");
	if (seat.has_value() == false) {
		record_outcome("register", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	RegistrationAnswer answer;
	answer.organisation_id = organisation;
	answer.seat_id = normalized_seat_id(*seat);
	const Json verification = payload.contains("contactVerification") ? payload.at("contactVerification") : Json::object();
	answer.masked_email = text_at(verification, "maskedEmail");
	record_outcome("register", "registered");
	return answer;

}

RegistrationAnswer create_organisation(const AccountPoster &post, const OrganisationRegistration &registration) {

	const auto text = [](const std::string &value) {
		const std::size_t first = value.find_first_not_of(" \t\r\n");
		if (first == std::string::npos) return std::string();
		return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
	};
	const std::string name = text(registration.legal_entity_name);
	const std::string country = text(registration.country);
	const std::string number = text(registration.registration_number);
	if (name.empty() || country.empty() || number.empty()) {
		throw AccountFailure(AccountFailureKind::InvalidInput, "Enter the organisation's legal name, country and registration number.");
	}
	if (name.size() > 200 || country.size() > 120 || number.size() > 120) {
		throw AccountFailure(AccountFailureKind::InvalidInput, "Shorten the organisation's details.");
	}
	const std::string email = normalized_email(registration.contact_email);
	if (!registration.accepts_licence_and_terms || !registration.acknowledges_privacy_notice ||
	    !registration.consents_to_personal_data || !registration.confirms_authority ||
	    !registration.confirms_shared_mailbox) {
		throw AccountFailure(AccountFailureKind::InvalidInput, "Accept each statement to register.");
	}

	/* - - - - - - */

	const Json agreements{{"softwareLicenceAccepted", registration.accepts_licence_and_terms},
	                      {"termsOfUseAccepted", registration.accepts_licence_and_terms},
	                      {"privacyNoticeAcknowledged", registration.acknowledges_privacy_notice},
	                      {"personalDataConsentAccepted", registration.consents_to_personal_data},
	                      {"authorityConfirmed", registration.confirms_authority},
	                      {"organisationEmailRepresentationAccepted", registration.confirms_shared_mailbox}};
	platform::HttpResponse response;
	try {
		response = post_or_fail(post, "/organisations/create",
			Json{{"legalEntityName", name}, {"country", country}, {"registrationNumber", number}, {"email", email},
			     {"agreements", agreements}, {"productUpdatesOptIn", registration.wants_product_updates}});
	} catch (const AccountFailure &failure) {
		record_outcome("create", failure.code());
		throw;
	}
	if (response.status == 409) {
		record_outcome("create", "already_registered");
		throw AccountFailure(AccountFailureKind::AlreadyRegistered,
			"An organisation with this registration number is registered already. Check the number, or sign in with that organisation's email.");
	}
	if (response.status == 429) {
		record_outcome("create", "too_many_attempts");
		throw AccountFailure(AccountFailureKind::TooManyAttempts,
			"Legal Machines paused registrations from here after many tries. Wait an hour, then try again.");
	}
	if (response.status == 400) {
		record_outcome("create", "invalid_input");
		throw AccountFailure(AccountFailureKind::InvalidInput, refusal_words(response.body, "Check the organisation's details."));
	}
	if (response.status < 200 || response.status >= 300) {
		record_outcome("create", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	const Json payload = parsed_or_fail(response.body);
	const std::optional<std::string> organisation = text_at(payload, "organisationId");
	const std::optional<std::string> seat = text_at(payload, "seatId");
	if (organisation.has_value() == false || seat.has_value() == false) {
		record_outcome("create", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	RegistrationAnswer answer;
	answer.organisation_id = normalized_organisation_id(*organisation);
	answer.seat_id = normalized_seat_id(*seat);
	const Json verification = payload.contains("contactVerification") ? payload.at("contactVerification") : Json::object();
	answer.masked_email = text_at(verification, "maskedEmail");
	record_outcome("create", "created");
	return answer;

}

namespace {

const char *kSetupExpired = "This setup has expired. Sign in to go on.";

// One step of a new organisation's setup (D-146): its answer, or the failure
// a person reads. `wrong_code` is the sentence for a code the service did not
// take, where the step takes one.
Json setup_step(const AccountPoster &post, const char *name, const std::string &path, const Json &body,
                const char *wrong_code = nullptr) {
	platform::HttpResponse response;
	try {
		response = post_or_fail(post, path, body);
	} catch (const AccountFailure &failure) {
		record_outcome(name, failure.code());
		throw;
	}
	if (response.status >= 200 && response.status < 300) {
		record_outcome(name, "done");
		return parsed_or_fail(response.body);
	}
	const Json refusal = Json::parse(response.body, nullptr, false);
	if (response.status == 401 && (text_at(refusal, "code") == std::optional<std::string>("setup_expired") || wrong_code == nullptr)) {
		record_outcome(name, "setup_expired");
		throw AccountFailure(AccountFailureKind::SetupExpired, kSetupExpired);
	}
	if (response.status == 401) {
		record_outcome(name, "wrong_code");
		throw AccountFailure(AccountFailureKind::WrongCode, wrong_code);
	}
	if (response.status == 429) {
		record_outcome(name, "too_many_attempts");
		throw AccountFailure(AccountFailureKind::TooManyAttempts, "Too many tries from here. Wait a few minutes, then try again.");
	}
	if (response.status == 400 || response.status == 409) {
		record_outcome(name, "invalid_input");
		throw AccountFailure(AccountFailureKind::InvalidInput, refusal_words(response.body, "Check it and try again."));
	}
	record_outcome(name, "service_unavailable");
	throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
}

Json setup_body(const std::string &organisation_id, const std::string &setup_token) {
	if (setup_token.empty() == true) throw AccountFailure(AccountFailureKind::SetupExpired, kSetupExpired);
	return Json{{"organisationId", normalized_organisation_id(organisation_id)}, {"setupToken", setup_token}};
}

} // namespace

void set_initial_password(const AccountPoster &post, const std::string &organisation_id,
                          const std::string &setup_token, const std::string &password) {
	std::size_t characters = 0;
	for (const unsigned char byte : password) {
		if ((byte & 0xC0) != 0x80) ++characters;
	}
	if (characters < 12) throw AccountFailure(AccountFailureKind::InvalidInput, "Choose a password of at least 12 characters.");
	if (characters > 200) throw AccountFailure(AccountFailureKind::InvalidInput, "Choose a shorter password.");
	Json body = setup_body(organisation_id, setup_token);
	body["password"] = password;
	setup_step(post, "setup_sign_in", "/organisations/setup/password", body);
}

TwoFactorKey start_two_factor(const AccountPoster &post, const std::string &organisation_id,
                              const std::string &setup_token) {
	const Json payload = setup_step(post, "setup_two_factor", "/organisations/setup/two-factor",
	                                setup_body(organisation_id, setup_token));
	const std::optional<std::string> secret = text_at(payload, "secret");
	const std::optional<std::string> uri = text_at(payload, "uri");
	if (secret.has_value() == false || uri.has_value() == false || uri->rfind("otpauth://totp/", 0) != 0) {
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	return TwoFactorKey{*secret, *uri};
}

TwoFactorOn confirm_two_factor(const AccountPoster &post, const std::string &organisation_id,
                               const std::string &setup_token, const std::string &code) {
	std::string digits;
	for (const char character : code) {
		if (character >= '0' && character <= '9') digits += character;
		else if (std::isspace(static_cast<unsigned char>(character)) == 0) digits += 'x';
	}
	if (digits.size() != 6 || digits.find('x') != std::string::npos) {
		throw AccountFailure(AccountFailureKind::InvalidInput, "Enter the six-digit code the app shows.");
	}
	Json body = setup_body(organisation_id, setup_token);
	body["code"] = digits;
	const Json payload = setup_step(post, "setup_two_factor_confirm", "/organisations/setup/two-factor/confirm", body,
	                                "That code doesn't match. Enter the one the app shows now.");
	TwoFactorOn on;
	on.state = text_at(payload, "state") == std::optional<std::string>("active") ? ContactConfirmation::Active
	                                                                            : ContactConfirmation::AwaitingApproval;
	on.challenge_id = text_at(payload, "challengeId").value_or("");
	on.organisation_name = text_at(payload, "organisationName");
	on.recovery_codes = recovery_codes_in(payload, "setup_two_factor_confirm");
	return on;
}

const char *to_string(ContactConfirmation confirmation) {
	switch (confirmation) {
		case ContactConfirmation::AwaitingApproval: return "awaiting_approval";
		case ContactConfirmation::Active: return "active";
	}
	return "awaiting_approval";
}

ContactConfirmed confirm_contact(const AccountPoster &post, const std::string &organisation_id,
                                 const std::string &code) {

	const std::string organisation = normalized_organisation_id(organisation_id);
	std::string digits;
	for (const char character : code) {
		if (character >= '0' && character <= '9') digits += character;
		else if (std::isspace(static_cast<unsigned char>(character)) == 0) digits += 'x';
	}
	if (digits.size() != 6 || digits.find('x') != std::string::npos) {
		throw AccountFailure(AccountFailureKind::InvalidInput, "Enter the six-digit code.");
	}

	platform::HttpResponse response;
	try {
		response = post_or_fail(post, "/organisations/verify-contact", Json{{"organisationId", organisation}, {"code", digits}});
	} catch (const AccountFailure &failure) {
		record_outcome("confirm_contact", failure.code());
		throw;
	}
	if (response.status == 429) {
		record_outcome("confirm_contact", "too_many_attempts");
		throw AccountFailure(AccountFailureKind::TooManyAttempts,
			"Too many wrong codes. Ask for a new code, then try again.");
	}
	if (response.status >= 400 && response.status < 500) {
		record_outcome("confirm_contact", "wrong_code");
		throw AccountFailure(AccountFailureKind::WrongCode, kWrongContactCode);
	}
	if (response.status < 200 || response.status >= 300) {
		record_outcome("confirm_contact", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	const Json payload = parsed_or_fail(response.body);
	// An organisation is in use only where the service says so; otherwise it
	// waits for Legal Machines' approval (D-030).
	ContactConfirmed confirmed;
	confirmed.state = flag_at(payload, "activated") == true && flag_at(payload, "awaitingApproval") == false
		? ContactConfirmation::Active
		: ContactConfirmation::AwaitingApproval;
	// A new organisation (D-146): the setup its confirmed email opens.
	const std::optional<std::string> setup = text_at(payload, "setup");
	const std::optional<std::string> token = text_at(payload, "setupToken");
	if (setup.has_value() == true && token.has_value() == true && (*setup == "password" || *setup == "two_factor")) {
		confirmed.setup = *setup;
		confirmed.setup_token = *token;
	}
	record_outcome("confirm_contact", confirmed.setup.empty() ? to_string(confirmed.state) : "setup");
	return confirmed;

}

void resend_contact_code(const AccountPoster &post, const std::string &organisation_id) {

	const std::string organisation = normalized_organisation_id(organisation_id);
	platform::HttpResponse response;
	try {
		response = post_or_fail(post, "/organisations/resend-contact-code", Json{{"organisationId", organisation}});
	} catch (const AccountFailure &failure) {
		record_outcome("resend_contact_code", failure.code());
		throw;
	}
	if (response.status == 429) {
		record_outcome("resend_contact_code", "too_many_attempts");
		throw AccountFailure(AccountFailureKind::TooManyAttempts, "A code was sent a moment ago. Wait a minute, then ask again.");
	}
	if (response.status == 400) {
		record_outcome("resend_contact_code", "invalid_input");
		throw AccountFailure(AccountFailureKind::InvalidInput, "This email is confirmed already. Sign in.");
	}
	if (response.status < 200 || response.status >= 300) {
		record_outcome("resend_contact_code", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	record_outcome("resend_contact_code", "sent");

}

SignInAnswer verify_two_factor(const AccountPoster &post, const std::string &challenge_id, const std::string &code) {

	const std::optional<std::string> sent = second_step_code(code);
	if (sent.has_value() == false) throw AccountFailure(AccountFailureKind::InvalidInput, "Enter the six-digit code.");
	const std::string &digits = *sent;
	if (challenge_id.empty() == true) {
		throw AccountFailure(AccountFailureKind::InvalidInput, "Sign in again to get a new code.");
	}

	platform::HttpResponse response;
	try {
		response = post_or_fail(post, "/auth/2fa/verify", Json{{"challengeId", challenge_id}, {"code", digits}});
	} catch (const AccountFailure &failure) {
		record_outcome("verify_two_factor", failure.code());
		throw;
	}

	if (response.status >= 200 && response.status < 300) {
		record_outcome("verify_two_factor", "accepted");
		SignInAnswer answer;
		answer.status = SignInStatus::SignedIn;
		const Json payload = Json::parse(response.body, nullptr, false);
		if (payload.is_discarded() == false) {
			answer.organisation_name = text_at(payload, "organisationName");
			answer.display_name = text_at(payload, "displayName");
			answer.role = text_at(payload, "role");
			// Signed in by a recovery code (D-218): the app asks for a new
			// authenticator app, and says how many codes are left.
			answer.used_recovery_code = flag_at(payload, "usedRecoveryCode");
			answer.recovery_codes_left = count_at(payload, "recoveryCodesLeft");
		}
		return answer;
	}
	if (response.status == 429) {
		record_outcome("verify_two_factor", "too_many_attempts");
		throw AccountFailure(AccountFailureKind::TooManyAttempts, kTooManyAttempts);
	}
	if (response.status >= 400 && response.status < 500) {
		record_outcome("verify_two_factor", "wrong_code");
		throw AccountFailure(AccountFailureKind::WrongCode,
			"That code was not accepted. Check it and try again, or sign in again for a new one.");
	}
	record_outcome("verify_two_factor", "service_unavailable");
	throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);

}

std::string exchange_node_session(const AccountPoster &post,
                                  const std::string &organisation_id,
                                  const std::string &seat_id,
                                  const std::string &password,
                                  const std::string &node_id,
                                  const std::string &challenge_id) {

	const std::string organisation = normalized_organisation_id(organisation_id);
	const std::string seat = normalized_seat_id(seat_id);
	if (password.empty() == true) throw AccountFailure(AccountFailureKind::InvalidInput, "Enter the password of this seat.");

	Json request{{"organisationId", organisation}, {"seatId", seat}, {"password", password}, {"nodeId", node_id}};
	if (challenge_id.empty() == false) request["challengeId"] = challenge_id;
	platform::HttpResponse response;
	try {
		response = post_or_fail(post, "/auth/node-session", request);
	} catch (const AccountFailure &failure) {
		record_outcome("node_session", failure.code());
		throw;
	}

	if (response.status == 401 || response.status == 403) {
		// What the refusal asks for, when it says: a code checked first, or the
		// organisation's email confirmed first. Anything else is the password.
		const Json refusal = Json::parse(response.body, nullptr, false);
		if (flag_at(refusal, "requiresTwoFactor") == true) {
			record_outcome("node_session", "code_required");
			throw AccountFailure(AccountFailureKind::CodeRequired,
				"Legal Machines needs a new sign-in code before this computer can download the protection model. "
				"Sign in again, and enter the code it sends.");
		}
		if (flag_at(refusal, "awaitingApproval") == true) {
			record_outcome("node_session", "awaiting_approval");
			throw AccountFailure(AccountFailureKind::AwaitingApproval, kAwaitingApproval);
		}
		if (flag_at(refusal, "requiresEmailVerification") == true) {
			record_outcome("node_session", "organisation_not_active");
			throw AccountFailure(AccountFailureKind::OrganisationNotActive,
				"Your organisation's email is not confirmed yet, so this computer cannot download the protection "
				"model. Open the message Legal Machines sent to it, confirm it, then sign in again.");
		}
		record_outcome("node_session", "wrong_credentials");
		throw AccountFailure(AccountFailureKind::WrongCredentials,
			"Legal Machines did not let this computer download the protection model. Sign in again.");
	}
	if (response.status == 429) {
		record_outcome("node_session", "too_many_attempts");
		throw AccountFailure(AccountFailureKind::TooManyAttempts, kTooManyAttempts);
	}
	if (response.status < 200 || response.status >= 300) {
		record_outcome("node_session", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}

	// A reply without a session is the service not answering properly - never
	// a reason to say anything the reply contained.
	const Json payload = parsed_or_fail(response.body);
	const std::optional<std::string> token = text_at(payload, "accessToken");
	if (token.has_value() == false) {
		record_outcome("node_session", "service_unavailable");
		throw AccountFailure(AccountFailureKind::ServiceUnavailable, kServiceUnavailable);
	}
	// A seat with no authenticator app is given a session that sets one up and
	// nothing else (D-215): the window leads its member there at once.
	if (text_at(payload, "twoFactorSetup") == std::optional<std::string>("required")) {
		record_outcome("node_session", "two_step_setup_required");
		tell_two_step_setup_required();
	} else {
		record_outcome("node_session", "accepted");
	}
	return *token;

}

} // namespace singularity::node
