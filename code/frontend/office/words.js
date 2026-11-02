// When something happened, as the pane says it: the time today, else the day.
export function when(at) {
	const moment = new Date(at);
	if (Number.isNaN(moment.getTime())) return "";
	const today = new Date();
	if (moment.toDateString() == today.toDateString()) {
		return new Intl.DateTimeFormat("en-US", { hour: "numeric", minute: "2-digit" }).format(moment);
	}
	return new Intl.DateTimeFormat("en-US", { month: "short", day: "numeric" }).format(moment);
}

// "Word", "Excel", "PowerPoint" or "Outlook".
export function appName(host) {
	return host == "excel" ? "Excel" : host == "powerpoint" ? "PowerPoint" : host == "outlook" ? "Outlook" : "Word";
}
