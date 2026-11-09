#include "Routing/ModelCall/ModelCall.hpp"

#include <algorithm>
#include <cctype>

namespace singularity::inference {

namespace {

std::string trimmed(const std::string &text) {
	const std::size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	const std::size_t last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

// A page in place of a reply - a proxy's, a gateway's or a provider's: an HTML
// document from its first character. Words with a tag in them are words.
bool isWebPage(const std::string &whole) {
	std::string head = whole.substr(0, 16);
	std::transform(head.begin(), head.end(), head.begin(), [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
	return head.rfind("<!doctype html", 0) == 0 || head.rfind("<html", 0) == 0;
}

bool hasType(const nlohmann::json &value, const std::string &type) {
	if (type == "object") return value.is_object();
	if (type == "array") return value.is_array();
	if (type == "string") return value.is_string();
	if (type == "integer") return value.is_number_integer();
	if (type == "number") return value.is_number();
	if (type == "boolean") return value.is_boolean();
	if (type == "null") return value.is_null();
	return true;
}

bool hasAnyType(const nlohmann::json &value, const nlohmann::json &declared) {
	if (declared.is_string() == true) return hasType(value, declared.get<std::string>());
	if (declared.is_array() == false) return true;
	for (const nlohmann::json &each : declared) {
		if (each.is_string() == true && hasType(value, each.get<std::string>()) == true) return true;
	}
	return false;
}

std::string placeOf(const std::string &path) {
	return path.empty() == true ? std::string("the object") : path;
}

std::string violationAt(const nlohmann::json &value, const nlohmann::json &schema, const std::string &path) {
	if (schema.is_object() == false) return std::string();
	if (schema.contains("type") == true && hasAnyType(value, schema.at("type")) == false)
		return placeOf(path) + " is not of the type asked for";
	if (schema.contains("enum") == true && schema.at("enum").is_array() == true) {
		const nlohmann::json &allowed = schema.at("enum");
		if (std::find(allowed.begin(), allowed.end(), value) == allowed.end()) return placeOf(path) + " is none of the values allowed";
	}
	if (value.is_object() == true) {
		const nlohmann::json properties = schema.contains("properties") == true && schema.at("properties").is_object() == true
			? schema.at("properties") : nlohmann::json::object();
		if (schema.contains("required") == true && schema.at("required").is_array() == true) {
			for (const nlohmann::json &key : schema.at("required")) {
				if (key.is_string() == true && value.contains(key.get<std::string>()) == false)
					return placeOf(path) + " has no " + key.get<std::string>();
			}
		}
		for (const auto &[key, item] : value.items()) {
			const std::string inside = path + "/" + key;
			if (properties.contains(key) == true) {
				const std::string broken = violationAt(item, properties.at(key), inside);
				if (broken.empty() == false) return broken;
				continue;
			}
			if (schema.contains("additionalProperties") == false) continue;
			const nlohmann::json &more = schema.at("additionalProperties");
			if (more.is_boolean() == true && more.get<bool>() == false) return placeOf(path) + " has " + key + ", which the form does not allow";
			const std::string broken = violationAt(item, more, inside);
			if (broken.empty() == false) return broken;
		}
	}
	if (value.is_array() == true && schema.contains("items") == true) {
		for (std::size_t index = 0; index < value.size(); ++index) {
			const std::string broken = violationAt(value.at(index), schema.at("items"), path + "/" + std::to_string(index));
			if (broken.empty() == false) return broken;
		}
	}
	return std::string();
}

} // namespace

const char *to_string(FailureClass failure) noexcept {
	switch (failure) {
		case FailureClass::Passing:
			return "passing";
		case FailureClass::RateLimited:
			return "rate_limited";
		case FailureClass::Refused:
			return "refused";
		case FailureClass::TooLargeForModel:
			return "too_large_for_model";
		case FailureClass::NotAllowedHere:
			return "not_allowed_here";
		case FailureClass::OverSpendAllowance:
			return "over_spend_allowance";
		case FailureClass::Stopped:
			return "stopped";
		case FailureClass::Closing:
			return "closing";
		case FailureClass::Unusable:
			return "unusable";
		case FailureClass::Incomplete:
			return "incomplete";
	}
	return "unusable";
}

FailureClass classify(providers::TaskErrorCode code, bool isStopped, bool isClosing) noexcept {
	// Closing first: the work begins again when Singularity opens.
	if (isClosing == true) return FailureClass::Closing;
	if (isStopped == true) return FailureClass::Stopped;
	switch (code) {
		case providers::TaskErrorCode::NetworkError:
			return FailureClass::Passing;
		case providers::TaskErrorCode::ProviderRateLimit:
			return FailureClass::RateLimited;
		case providers::TaskErrorCode::ContextCapacityExceeded:
			return FailureClass::TooLargeForModel;
		// Protection that cannot run, or a leak it caught: nothing may leave.
		case providers::TaskErrorCode::PrivacyProtectionUnavailable:
		case providers::TaskErrorCode::ProtectedPayloadLeakDetected:
			return FailureClass::NotAllowedHere;
		// A reply that could not be read: an invalid protected answer, or a body
		// that was no provider's answer at all (f-unusable-reply).
		case providers::TaskErrorCode::ProtectedResponseInvalid:
		case providers::TaskErrorCode::Unknown:
			return FailureClass::Unusable;
		case providers::TaskErrorCode::ResponseIncomplete:
			return FailureClass::Incomplete;
		// A remote model for work that stays here is refused today as an
		// unsupported model; M1b gives the boundary's refusal its own class.
		case providers::TaskErrorCode::ProviderAuthentication:
		case providers::TaskErrorCode::UnsupportedModel:
		case providers::TaskErrorCode::ProviderRejectedRequest:
		case providers::TaskErrorCode::TargetChanged:
		case providers::TaskErrorCode::EvidenceChanged:
		case providers::TaskErrorCode::ChangeRejected:
			return FailureClass::Refused;
	}
	return FailureClass::Unusable;
}

bool is_retried_within_the_call(FailureClass failure) noexcept {
	return failure == FailureClass::Passing;
}

ReplyCheck check_reply(const std::string &text, const ReplyContract &contract) {
	ReplyCheck checked;
	const std::string whole = trimmed(text);
	if (whole.empty() == true) {
		checked.why_unusable = "it was empty";
		return checked;
	}
	if (contract.form == ReplyForm::Text) {
		checked.reply = Reply{text, std::nullopt};
		return checked;
	}
	if (contract.form == ReplyForm::StructuredOrText && isWebPage(whole) == true) {
		checked.why_unusable = "it was a web page, not an answer";
		return checked;
	}
	// Words, where the form allows them: plain words never begin with the
	// object's brace (the executor has already taken a whole object out of a
	// fence or a sentence around it).
	const bool isObjectBegun = whole.front() == '{';
	if (contract.form == ReplyForm::StructuredOrText && isObjectBegun == false) {
		checked.reply = Reply{text, std::nullopt};
		return checked;
	}
	const nlohmann::json object = nlohmann::json::parse(whole, nullptr, false);
	if (object.is_object() == false) {
		checked.why_unusable = contract.form == ReplyForm::Structured ? std::string("it was not the object asked for")
			: std::string("its answer in the form asked for was cut off or broken");
		return checked;
	}
	const std::string broken = schema_violation(object, contract.schema);
	if (broken.empty() == false) {
		checked.why_unusable = "its answer does not keep to the form asked for: " + broken;
		return checked;
	}
	checked.reply = Reply{text, object};
	return checked;
}

std::string schema_violation(const nlohmann::json &value, const nlohmann::json &schema) {
	return violationAt(value, schema, std::string());
}

} // namespace singularity::inference
