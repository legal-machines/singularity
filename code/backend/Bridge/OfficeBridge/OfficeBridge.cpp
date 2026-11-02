#include "Bridge/OfficeBridge/OfficeBridge.hpp"

#include "DocumentIntelligence/EmailDraft/EmailDraft.hpp"
#include "Office/OfficeFiles/OfficeFiles.hpp"
#include "Office/OfficePaths/OfficePaths.hpp"
#include "Office/OfficeService/OfficeService.hpp"

#include <stdexcept>

namespace singularity::bridge::office_bridge {
namespace {

using Json = nlohmann::json;

std::string required_text(const Json &params, const char *name) {
	if (!params.contains(name) || !params.at(name).is_string() || params.at(name).get<std::string>().empty()) {
		throw std::invalid_argument(std::string(name) + " is required.");
	}
	return params.at(name).get<std::string>();
}

std::string optional_text(const Json &params, const char *name) {
	if (!params.contains(name) || params.at(name).is_null()) return std::string();
	if (!params.at(name).is_string()) throw std::invalid_argument(std::string(name) + " must be text.");
	return params.at(name).get<std::string>();
}

Json reveal_json(const office::RevealRequest &request) {
	return Json{{"revealId", request.id}, {"matterId", request.matter_id}, {"documentId", request.document_id},
		{"section", request.section}, {"heading", request.heading}, {"quote", request.quote}, {"requestedAt", request.requested_at}};
}

Json status_json(const office::OfficeStatus &status) {
	return Json{{"supported", status.supported}, {"connected", status.connected}, {"listening", status.listening},
		{"port", status.port}, {"certificate", status.certificate}, {"certificateEnds", status.certificate_ends},
		{"documentsRegistered", status.documents_registered}, {"outlookManifest", status.outlook_manifest},
		{"connectedAt", status.connected_at}, {"problem", status.problem}};
}

} // namespace

Json list_clients(const Json &) {
	Json pending = Json::array();
	for (const office::PendingPairing &request : office::office_pairings()) {
		pending.push_back(Json{{"pairingId", request.id}, {"client", office::word_of(request.kind)}, {"label", request.label},
			{"code", request.code}, {"requestedAt", request.requested_at}});
	}
	Json clients = Json::array();
	for (const office::PairedClient &client : office::office_clients()) {
		clients.push_back(Json{{"clientId", client.id}, {"client", office::word_of(client.kind)}, {"label", client.label},
			{"pairedAt", client.paired_at}, {"lastUsedAt", client.last_used_at}, {"expiresAt", client.expires_at}});
	}
	return Json{{"pending", pending}, {"clients", clients}};
}

Json decide_pairing(const Json &params) {
	const std::string pairing_id = required_text(params, "pairingId");
	if (!params.contains("allow") || !params.at("allow").is_boolean()) throw std::invalid_argument("allow is required.");
	return Json{{"decided", office::decide_office_pairing(pairing_id, params.at("allow").get<bool>())}};
}

Json remove_client(const Json &params) {
	return Json{{"removed", office::remove_office_client(required_text(params, "clientId"))}};
}

Json get_status(const Json &) {
	return status_json(office::office_status());
}

Json connect(const Json &) {
	return status_json(office::connect_office());
}

Json disconnect(const Json &) {
	return status_json(office::disconnect_office());
}

Json open_app(const Json &params) {
	const auto app = office::office_app_of(required_text(params, "app"));
	if (!app) throw std::invalid_argument("app is word, excel, powerpoint or outlook.");
	const office::OpenedApp opened = office::open_office_app(*app);
	return Json{{"app", office::word_of(*app)}, {"opened", opened.opened}, {"problem", opened.problem}};
}

Json describe_file(const Json &params) {
	const std::string location = params.value("location", std::string());
	const office::DescribedFile described = office::describe_file(location);
	Json place = nullptr;
	// A place the person asked to see in this document a moment ago, from the
	// window, for the pane that opens with it.
	Json reveal = nullptr;
	if (described.place) {
		place = Json{{"matterId", described.place->matter_id}, {"matterName", described.place->matter_name},
			{"relativePath", described.place->relative_path}, {"documentId", described.place->document_id}};
		if (const auto pending = office::pending_reveal(described.place->document_id)) reveal = reveal_json(*pending);
	}
	return Json{{"kind", described.kind}, {"name", described.name}, {"place", place}, {"reveal", reveal}};
}

Json file_into_matter(const Json &params) {
	const std::string matter_id = required_text(params, "matterId");
	const std::string name = required_text(params, "name");
	const std::string encoded = required_text(params, "base64");
	const auto bytes = office::decode_base64(encoded);
	if (!bytes) throw std::invalid_argument("The file's bytes could not be read.");
	const office::Filed filed = office::file_into_matter(matter_id, name, *bytes);
	return Json{{"matterId", filed.matter_id}, {"relativePath", filed.relative_path}, {"name", filed.name}};
}

Json document_content(const Json &params) {
	const office::DocumentContent content = office::document_content(required_text(params, "matterId"), required_text(params, "documentId"));
	return Json{{"name", content.name}, {"sizeBytes", content.bytes.size()}, {"base64", office::encode_base64(content.bytes)}};
}

Json email_draft(const Json &params) {
	const office::DocumentContent content = office::document_content(required_text(params, "matterId"), required_text(params, "documentId"));
	std::string extension = std::filesystem::u8path(content.name).extension().u8string();
	for (char &character : extension) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	if (extension != ".eml") throw std::invalid_argument("That document is not an email draft.");
	const document_intelligence::ReadEmail read = document_intelligence::readEmailText(content.bytes);
	if (!read.isRead) throw std::runtime_error("That email draft could not be read.");
	Json attachments = Json::array();
	for (const auto &[name, bytes] : read.attachments)
		attachments.push_back(Json{{"name", name}, {"sizeBytes", bytes.size()}, {"base64", office::encode_base64(bytes)}});
	return Json{{"name", content.name}, {"isUnsent", read.isUnsent}, {"to", read.to}, {"cc", read.cc}, {"subject", read.subject},
		{"body", read.body}, {"attachments", attachments}};
}

Json open_document(const Json &params) {
	office::open_document(required_text(params, "matterId"), required_text(params, "documentId"));
	return Json{{"opened", true}};
}

Json reveal(const Json &params) {
	const office::RevealRequest request = office::reveal_in_office(required_text(params, "matterId"),
		required_text(params, "documentId"), optional_text(params, "section"), optional_text(params, "heading"),
		optional_text(params, "quote"));
	return Json{{"revealId", request.id}, {"opened", true}};
}

} // namespace singularity::bridge::office_bridge
