#include "Bridge/OrganisationBridge/OrganisationBridge.hpp"

#include "Organisation/OrganisationProduction/OrganisationProduction.hpp"

#include <filesystem>

namespace singularity::bridge::organisation_bridge {
namespace {

using Json = nlohmann::json;
using namespace singularity::organisation;

Json strings(const std::vector<std::string> &values) {
	Json list = Json::array();
	for (const std::string &value : values) list.push_back(value);
	return list;
}

Json viewer_json(const Viewer &viewer) {
	return Json{{"memberId", viewer.member_id}, {"name", viewer.name}, {"organisationId", viewer.organisation_id},
		{"organisationName", viewer.organisation_name}, {"seatId", viewer.seat_id}, {"deviceId", viewer.device_id},
		{"position", viewer.position}, {"practice", viewer.practice},
		{"photo", viewer.photo.empty() ? Json(nullptr) : Json(viewer.photo)},
		{"may", Json{{"decideChanges", viewer.decides_changes}, {"answerQuestions", viewer.answers_questions},
			{"giveWork", viewer.gives_work}, {"manageColleagues", viewer.manages_colleagues},
			{"connectModels", viewer.connects_models}, {"manageSeats", viewer.manages_seats},
			{"resolveConflicts", viewer.resolves_conflicts}}}};
}

Json record_json(const MemberRecord &record) {
	return Json{{"recentRuns", record.recent_runs}, {"recentRunsCompleted", record.recent_runs_completed},
		{"recentRunsFailed", record.recent_runs_failed}, {"workGiven", record.work_given}, {"workDone", record.work_done},
		{"changesWaiting", record.changes_waiting}, {"decisionsWaiting", record.decisions_waiting}, {"spend", record.spend},
		{"spendCurrency", record.spend_currency}, {"spendMonth", record.spend_month}, {"lastActiveAt", record.last_active_at}};
}

Json member_json(const Member &member) {
	return Json{{"id", member.id}, {"kind", member.kind}, {"name", member.name}, {"isViewer", member.is_viewer},
		{"role", member.role}, {"roleName", member.role_name}, {"state", member.state}, {"answersTo", member.answers_to},
		{"character", member.character}, {"presence", member.presence}, {"runsHere", member.runs_here},
		{"areaIds", strings(member.area_ids)},
		{"matterIds", strings(member.matter_ids)}, {"record", record_json(member.record)}};
}

Json area_json(const Area &area) {
	return Json{{"id", area.id}, {"memberId", area.member_id}, {"matterId", area.matter_id}, {"matterName", area.matter_name},
		{"folder", area.folder}, {"role", area.role}, {"mode", area.mode}, {"trigger", area.trigger},
		{"playbookId", area.playbook_id}, {"playbookName", area.playbook_name}, {"instruction", area.instruction}};
}

Json assignment_json(const Assignment &assignment) {
	return Json{{"id", assignment.id}, {"kind", assignment.kind}, {"memberId", assignment.member_id},
		{"givenBy", assignment.given_by}, {"matterId", assignment.matter_id}, {"chatId", assignment.chat_id},
		{"operationId", assignment.operation_id}, {"title", assignment.title}, {"state", assignment.state},
		{"startedAt", assignment.started_at}, {"updatedAt", assignment.updated_at}};
}

Json decision_json(const Decision &decision) {
	return Json{{"id", decision.id}, {"kind", decision.kind}, {"matterId", decision.matter_id},
		{"matterName", decision.matter_name}, {"chatId", decision.chat_id}, {"operationId", decision.operation_id},
		{"objectId", decision.object_id}, {"title", decision.title}, {"askedBy", decision.asked_by},
		{"holder", decision.holder}, {"waitingSince", decision.waiting_since}, {"decidableHere", decision.decidable_here},
		{"elsewhereReason", decision.elsewhere_reason}};
}

} // namespace

Json get(const Json &) {
	const OrganisationView view = organisation::read();
	Json members = Json::array();
	for (const Member &member : view.members) members.push_back(member_json(member));
	Json areas = Json::array();
	for (const Area &area : view.areas) areas.push_back(area_json(area));
	Json assignments = Json::array();
	for (const Assignment &assignment : view.assignments) assignments.push_back(assignment_json(assignment));
	Json decisions = Json::array();
	for (const Decision &decision : view.decisions) decisions.push_back(decision_json(decision));
	return Json{{"viewer", viewer_json(view.viewer)}, {"members", members}, {"areas", areas},
		{"assignments", assignments}, {"decisions", decisions}, {"readAt", view.read_at}};
}

nlohmann::json viewer() {
	return viewer_json(organisation::viewer());
}

nlohmann::json set_own_office(const nlohmann::json &params) {
	MemberOffice office;
	if (params.contains("position") == true && params.at("position").is_string() == true) office.position = params.at("position").get<std::string>();
	if (params.contains("practice") == true && params.at("practice").is_string() == true) office.practice = params.at("practice").get<std::string>();
	(void)organisation::set_own_office(office);
	return viewer_json(organisation::viewer());
}

nlohmann::json set_own_photo(const nlohmann::json &params) {
	const auto photo = params.find("photo");
	(void)organisation::set_own_photo(photo != params.end() && photo->is_string() ? photo->get<std::string>() : std::string());
	return viewer_json(organisation::viewer());
}

namespace {

Json profile_json(const organisation::SeatProfile &profile) {
	return Json{{"seatId", profile.seat_id}, {"name", profile.name}, {"rank", profile.rank}, {"title", profile.title},
		{"practice", profile.practice}, {"about", profile.about},
		{"photo", profile.photo.empty() ? Json(nullptr) : Json(profile.photo)}, {"at", profile.at},
		{"state", profile.state}};
}

} // namespace

nlohmann::json seat_profiles(const nlohmann::json &) {
	Json profiles = Json::array();
	for (const organisation::SeatProfile &profile : organisation::seat_profiles()) profiles.push_back(profile_json(profile));
	return Json{{"profiles", std::move(profiles)}};
}

nlohmann::json save_seat_profile(const nlohmann::json &params) {
	const auto text = [&params](const char *key) {
		return params.contains(key) && params.at(key).is_string() ? params.at(key).get<std::string>() : std::string();
	};
	organisation::SeatProfile profile;
	profile.seat_id = text("seatId");
	profile.name = text("name");
	profile.rank = text("rank");
	profile.title = text("title");
	profile.practice = text("practice");
	profile.about = text("about");
	profile.photo = text("photo");
	return profile_json(organisation::save_profile_of_seat(profile));
}

nlohmann::json set_seat_state(const nlohmann::json &params,
	const std::function<void(const std::string &seat_id, const std::string &state, const std::string &password)> &at_legal_machines) {
	const auto text = [&params](const char *key) {
		return params.contains(key) && params.at(key).is_string() ? params.at(key).get<std::string>() : std::string();
	};
	const std::string seat = text("seatId");
	const std::string state = text("state");
	const std::string password = text("password");
	return profile_json(organisation::set_seat_state(seat, state, [&]() {
		if (at_legal_machines) at_legal_machines(seat, state, password);
	}));
}

nlohmann::json own_photo_picture(const std::optional<std::string> &path) {
	if (!path || path->empty()) return nullptr;
	return Json{{"picture", organisation::picture_from_file(std::filesystem::u8path(*path))}};
}

} // namespace singularity::bridge::organisation_bridge
