#include "Provider/CredentialStore/CredentialStore.hpp"

#include <libsecret/secret.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>

#ifndef NATIVE_APP_SLUG
#error "NATIVE_APP_SLUG must name the application whose keys these are"
#endif

namespace singularity::platform {
namespace {

// Keys live in the desktop keyring - the Secret Service that GNOME Keyring,
// KDE Wallet and KeePassXC provide - through libsecret: encrypted by the
// keyring, unlocked with the desktop session, and never in a file of Singularity's
// own. Where no keyring can be reached, a key is refused; it is
// never kept anywhere else (AG-SEC-1).
//
// Each key is one item of this schema, found by the application's slug (so a
// development build and a test keep apart from the released app, as their
// settings folders do) and the provider's identifier.
const SecretSchema *credential_schema() {
	static const SecretSchema schema = [] {
		SecretSchema value{};
		value.name = "ai.legalmachines.singularity.Credential";
		value.flags = SECRET_SCHEMA_NONE;
		value.attributes[0] = {"application", SECRET_SCHEMA_ATTRIBUTE_STRING};
		value.attributes[1] = {"provider", SECRET_SCHEMA_ATTRIBUTE_STRING};
		return value;
	}();
	return &schema;
}

void require_valid_identifier(const std::string &provider_id) {
	if (provider_id.empty() || !std::all_of(provider_id.begin(), provider_id.end(), [](unsigned char value) {
				return std::isalnum(value) || value == '_' || value == '-';
			})) {
		throw std::runtime_error("Invalid provider credential identifier.");
	}
}

struct HashTableRelease {
	void operator()(GHashTable *table) const { g_hash_table_unref(table); }
};
using Attributes = std::unique_ptr<GHashTable, HashTableRelease>;

Attributes attributes_for(const std::string &provider_id) {
	Attributes attributes(g_hash_table_new(g_str_hash, g_str_equal));
	g_hash_table_insert(attributes.get(), const_cast<char *>("application"), const_cast<char *>(NATIVE_APP_SLUG));
	g_hash_table_insert(attributes.get(), const_cast<char *>("provider"), const_cast<char *>(provider_id.c_str()));
	return attributes;
}

struct ErrorRelease {
	void operator()(GError *error) const { g_error_free(error); }
};
using Error = std::unique_ptr<GError, ErrorRelease>;

enum class Action { kReading, kSaving, kRemoving };

// The D-Bus name of an error a peer sent back, or "" for one raised here.
std::string remote_error_name(const GError *error) {
	if (error == nullptr) return {};
	gchar *name = g_dbus_error_get_remote_error(error);
	if (name == nullptr) return {};
	std::string result(name);
	g_free(name);
	return result;
}

std::string error_summary(const GError *error) {
	if (error == nullptr) return "domain=unknown code=0";
	const char *domain = g_quark_to_string(error->domain);
	const std::string remote = remote_error_name(error);
	return "domain=" + std::string(domain == nullptr ? "unknown" : domain) + " code=" + std::to_string(error->code)
		+ (remote.empty() ? std::string() : " " + remote);
}

// The keyring is there but locked, and its unlock was refused or dismissed.
bool is_locked(const GError *error) {
	if (error == nullptr) return true;
	if (g_error_matches(error, SECRET_ERROR, SECRET_ERROR_IS_LOCKED) == TRUE) return true;
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) == TRUE) return true;
	const std::string remote = remote_error_name(error);
	return remote == "org.freedesktop.Secret.Error.IsLocked";
}

// No keyring answers: no session bus (a headless session), or a bus on which
// nothing provides the Secret Service (a desktop without a keyring).
bool is_unreachable(const GError *error) {
	if (error == nullptr) return false;
	const std::string remote = remote_error_name(error);
	if (remote.empty() == false) {
		return remote == "org.freedesktop.DBus.Error.ServiceUnknown"
			|| remote == "org.freedesktop.DBus.Error.NameHasNoOwner"
			|| remote.rfind("org.freedesktop.DBus.Error.Spawn.", 0) == 0;
	}
	return error->domain != SECRET_ERROR;
}

// What went wrong, said to the person it happens to, as
// CredentialStoreMacOS.mm says it: the cause, then the one thing to do. The
// technical summary stays at the end for whoever diagnoses something rarer.
// No key is ever part of this text.
std::string readable_failure(const GError *error, Action action) {
	const std::string detail = " (" + error_summary(error) + ")";
	if (is_locked(error) == true) {
		switch (action) {
		case Action::kSaving:
			return "This computer's keyring is locked, so the model credential was not saved. Unlock the keyring, "
			       "then try again."
				+ detail;
		case Action::kReading:
			return "This computer's keyring is locked, so Singularity cannot read the saved model credential. "
			       "Unlock the keyring, then try again."
				+ detail;
		case Action::kRemoving:
			return "This computer's keyring is locked, so the model credential was not removed. Unlock the keyring, "
			       "then try again."
				+ detail;
		}
	}
	if (is_unreachable(error) == true) {
		switch (action) {
		case Action::kSaving:
			return "Singularity could not find this computer's keyring, so the model credential was not saved. "
			       "Singularity keeps credentials only in the desktop keyring (such as GNOME Keyring or KDE Wallet), "
			       "never in a file. Open Singularity in a desktop session that has a keyring, then try again."
				+ detail;
		case Action::kReading:
			return "Singularity could not find this computer's keyring, so it cannot read the saved model "
			       "credential. Open Singularity in a desktop session that has a keyring (such as GNOME Keyring or "
			       "KDE Wallet), then try again."
				+ detail;
		case Action::kRemoving:
			return "Singularity could not find this computer's keyring, so the model credential was not removed. "
			       "Open Singularity in a desktop session that has a keyring, then try again."
				+ detail;
		}
	}
	switch (action) {
	case Action::kSaving:
		return "This computer's keyring did not save the model credential. Nothing was sent and nothing was changed."
			+ detail;
	case Action::kReading:
		return "This computer's keyring did not return the saved model credential. Nothing was sent." + detail;
	case Action::kRemoving:
		break;
	}
	return "This computer's keyring did not remove the model credential." + detail;
}

} // namespace

// A key that cannot be read now is most often behind a locked keyring.
std::string credential_unreadable_explanation(const std::string &provider_name) {
	return provider_name + " could not be opened on this computer. Unlock this computer's keyring, or reconnect "
		+ provider_name + ".";
}

std::optional<std::string> get_provider_credential(const std::string &provider_id) {
	require_valid_identifier(provider_id);
	const Attributes attributes = attributes_for(provider_id);
	GError *raw = nullptr;
	SecretValue *value = secret_password_lookupv_binary_sync(credential_schema(), attributes.get(), nullptr, &raw);
	const Error error(raw);
	if (error != nullptr) {
		if (value != nullptr) secret_value_unref(value);
		throw std::runtime_error(readable_failure(error.get(), Action::kReading));
	}
	if (value == nullptr) return std::nullopt;
	gsize length = 0;
	const gchar *bytes = secret_value_get(value, &length);
	std::string credential(bytes == nullptr ? "" : bytes, bytes == nullptr ? 0 : length);
	secret_value_unref(value);
	return credential;
}

void set_provider_credential(const std::string &provider_id, const std::string &credential) {
	require_valid_identifier(provider_id);
	const Attributes attributes = attributes_for(provider_id);
	// The name the keyring's own window (Passwords and Keys, KWalletManager) shows.
#ifdef NATIVE_APP_DISPLAY_NAME
	const std::string label = std::string(NATIVE_APP_DISPLAY_NAME) + " credential: " + provider_id;
#else
	const std::string label = std::string(NATIVE_APP_SLUG) + " credential: " + provider_id;
#endif
	// The keyring's own memory holds the copy; libsecret clears it when freed.
	SecretValue *value = secret_value_new(credential.data(), static_cast<gssize>(credential.size()), "text/plain");
	GError *raw = nullptr;
	const gboolean stored = secret_password_storev_binary_sync(credential_schema(), attributes.get(),
		SECRET_COLLECTION_DEFAULT, label.c_str(), value, nullptr, &raw);
	secret_value_unref(value);
	const Error error(raw);
	if (stored == FALSE) throw std::runtime_error(readable_failure(error.get(), Action::kSaving));
}

void delete_provider_credential(const std::string &provider_id) {
	require_valid_identifier(provider_id);
	const Attributes attributes = attributes_for(provider_id);
	GError *raw = nullptr;
	// FALSE without an error only means there was nothing to remove.
	(void)secret_password_clearv_sync(credential_schema(), attributes.get(), nullptr, &raw);
	const Error error(raw);
	if (error != nullptr) throw std::runtime_error(readable_failure(error.get(), Action::kRemoving));
}

} // namespace singularity::platform
