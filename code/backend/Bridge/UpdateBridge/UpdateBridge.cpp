#include "Bridge/UpdateBridge/UpdateBridge.hpp"

#include "Node/AppUpdate/AppUpdate.hpp"

namespace singularity::bridge::update_bridge {

namespace {

using Json = nlohmann::json;

// What the window shows: the state in its words, the version here and the one
// found - its number, date, notes, installer name and size - and a download's
// progress. Never the address, the checksum or the signature: they are the
// backend's to check.
Json status_json(const node::update::Status &status) {
	Json release = nullptr;
	if (status.release) {
		release = Json{{"version", status.release->version}, {"publishedAt", status.release->published_at},
			{"notes", status.release->notes}, {"fileName", status.release->file_name}, {"bytes", status.release->bytes}};
	}
	return Json{{"state", status.state}, {"currentVersion", status.current_version}, {"checkedAt", status.checked_at},
		{"received", status.received}, {"installer", status.installer}, {"detail", status.detail}, {"release", std::move(release)}};
}

} // namespace

nlohmann::json status(const nlohmann::json &) {
	return status_json(node::update::status());
}

nlohmann::json check(const nlohmann::json &) {
	return status_json(node::update::check_now());
}

nlohmann::json download(const nlohmann::json &) {
	return status_json(node::update::download_now());
}

nlohmann::json show(const nlohmann::json &) {
	node::update::show_installer();
	return status_json(node::update::status());
}

} // namespace singularity::bridge::update_bridge
