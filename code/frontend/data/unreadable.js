// A Matter whose data can't be read just now - a write caught midway, a file a
// sync client holds, a folder out of reach - is refused by the backend with one
// of these codes (matters::ProjectDataError): what the window shows of it
// stays, and it is read again.
export function isUnreadableNow(error) {
	return error?.code == "project_data_damaged" || error?.code == "project_data_unavailable";
}
