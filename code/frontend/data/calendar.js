// MOCK DATA - Evelyn Hart's calendars at Marlowe Finch LLP, around Friday,
// October 23, 2026 (AG-12, PROD-2). Every name and event is fictional. Its
// deadlines and meetings are those of the Matters' threads; an AI colleague
// keeps its own scheduled work here as a person keeps a meeting; the time the
// viewer recorded comes from data/billing.js. The connected accounts are
// simulated: no Microsoft or Google account is reached (D-079). Nothing here
// is read from, or written to, the backend.

// The calendars, each in a pigment of Folio's palette (styles/tokens.css).
export const calendars = [
	{ id: "cal_mine", name: "Evelyn Hart", color: "#2f56b0", group: "mine" },
	{ id: "cal_deadlines", name: "Deadlines, kept by Griffin", color: "#c43a24", group: "mine" },
	{ id: "cal_colleagues", name: "AI colleagues' schedules", color: "#7a4b97", group: "mine" },
	{ id: "cal_time", name: "Time recorded", color: "#2f8a6e", group: "mine" },
	{ id: "cal_away", name: "Out of office", color: "#8a8fa0", group: "mine" },
	{ id: "cal_outlook", name: "Calendar", color: "#4a78c2", group: "other", account: "outlook" },
	{ id: "cal_holidays", name: "Holidays in United States", color: "#8b6a4b", group: "other" },
];

// The accounts the member's calendars and mail connect to (D-079).
export const accounts = [
	{ id: "outlook", provider: "Microsoft 365", product: "Outlook", address: "evelyn.hart@marlowefinch.example", connected: true },
	{ id: "google", provider: "Google", product: "Google Calendar", address: "evelyn.hart.personal@gmail.example", connected: false },
];

// What a Google account brings when the member connects it, in the mock.
export const googleSample = {
	calendar: { id: "cal_google", name: "Personal", color: "#c2851a", group: "other", account: "google" },
	events: [
		{ id: "ev_g_run", calendarId: "cal_google", title: "Run in Central Park", start: "2026-10-24T08:00", end: "2026-10-24T09:00" },
		{ id: "ev_g_dinner", calendarId: "cal_google", title: "Dinner with Sam", start: "2026-10-24T19:30", end: "2026-10-24T21:30", location: "West Village" },
		{ id: "ev_g_dentist", calendarId: "cal_google", title: "Dentist", start: "2026-10-29T08:00", end: "2026-10-29T08:45" },
	],
};

export const calendarEvents = [
	// Evelyn's own.
	{ id: "ev_halvane_weekly", calendarId: "cal_mine", title: "Weekly call with Halvane's legal team", start: "2026-09-14T11:00", end: "2026-09-14T11:45", repeat: { every: "week", until: "2026-12-21" }, skip: ["2026-10-12"], memberIds: ["m_evelyn", "m_noah", "m_priya", "m_juno"], location: "Microsoft Teams", description: "Status of every open Halvane Matter, Matter by Matter." },
	{ id: "ev_associates", calendarId: "cal_mine", title: "Associates' lunch", start: "2026-09-17T12:30", end: "2026-09-17T13:30", repeat: { every: "week", until: "2026-12-17" }, location: "32nd floor, Hudson room" },
	{ id: "ev_orbis_call", calendarId: "cal_mine", title: "Negotiation call with Orbis's counsel", matterId: "x_orbis", start: "2026-10-20T14:00", end: "2026-10-20T15:00", memberIds: ["m_noah", "m_evelyn", "m_themis"], location: "Microsoft Teams", threadId: "t_orbis" },
	{ id: "ev_orbis_cap", calendarId: "cal_mine", title: "Orbis liability cap, with Noah", matterId: "x_orbis", start: "2026-10-23T12:00", end: "2026-10-23T12:30", memberIds: ["m_evelyn", "m_noah"], threadId: "t_orbis" },
	{ id: "ev_kestrel_review", calendarId: "cal_mine", title: "Final review of the answer with local counsel", matterId: "x_kestrel", start: "2026-10-27T15:00", end: "2026-10-27T16:00", memberIds: ["m_priya", "m_evelyn", "m_iris"], location: "Microsoft Teams", threadId: "t_kestrel" },
	{ id: "ev_sablewood_sign", calendarId: "cal_mine", title: "Sablewood NDA: signature, if Sablewood accepts v2", matterId: "x_sablewood", start: "2026-10-30T10:00", end: "2026-10-30T10:30", memberIds: ["m_evelyn", "m_juno", "m_lena"], threadId: "t_sablewood" },
	{ id: "ev_gabriel_checkin", calendarId: "cal_mine", title: "Partner check-in with Gabriel", start: "2026-10-23T09:00", end: "2026-10-23T09:20", memberIds: ["m_evelyn", "m_gabriel"], location: "Gabriel's office" },
	{ id: "ev_exhibit_b", calendarId: "cal_mine", title: "Exhibit B, with Halvane's sales team", matterId: "x_sablewood", start: "2026-10-23T14:30", end: "2026-10-23T15:00", memberIds: ["m_evelyn", "m_lena"], location: "Microsoft Teams", threadId: "t_sablewood" },
	{ id: "ev_partner_review", calendarId: "cal_mine", title: "Mid-year review with Gabriel", start: "2026-11-05T16:00", end: "2026-11-05T17:00", memberIds: ["m_evelyn", "m_gabriel"], location: "Gabriel's office" },

	// Deadlines, kept by Griffin.
	{ id: "ev_orbis_reply", calendarId: "cal_deadlines", title: "Orbis expects Halvane's position on the liability cap", matterId: "x_orbis", allDay: true, startDay: "2026-10-23", endDay: "2026-10-23", threadId: "t_orbis" },
	{ id: "ev_kestrel_answer", calendarId: "cal_deadlines", title: "Answer to Kestrel's complaint due", matterId: "x_kestrel", allDay: true, startDay: "2026-10-28", endDay: "2026-10-28", threadId: "t_kestrel", description: "D. Del. The answer and affirmative defenses, filed by Delaware local counsel." },
	{ id: "ev_kestrel_conference", calendarId: "cal_deadlines", title: "Kestrel v. Halvane: scheduling conference", matterId: "x_kestrel", start: "2026-11-12T10:00", end: "2026-11-12T10:30", location: "U.S. District Court, D. Del., by telephone", threadId: "t_kestrel" },

	// The AI colleagues' scheduled work.
	{ id: "ev_kestrel_remind", calendarId: "cal_colleagues", title: "Griffin reminds Priya and local counsel of the answer", matterId: "x_kestrel", start: "2026-10-26T09:00", end: "2026-10-26T09:30", memberIds: ["m_iris"], threadId: "t_kestrel" },
	{ id: "ev_juno_friday", calendarId: "cal_colleagues", title: "Unicorn sends Evelyn the open NDAs", start: "2026-10-16T16:00", end: "2026-10-16T16:15", repeat: { every: "week", until: "2026-12-25" }, memberIds: ["m_juno"] },
	{ id: "ev_juno_desk", calendarId: "cal_colleagues", title: "Unicorn reviews the NDA desk's queue", start: "2026-10-19T08:00", end: "2026-10-19T08:30", repeat: { every: "week", until: "2026-12-21" }, memberIds: ["m_juno"] },

	// Out of office.
	{ id: "ev_priya_away", calendarId: "cal_away", title: "Priya Raman out of office", allDay: true, startDay: "2026-10-21", endDay: "2026-10-26", memberIds: ["m_priya"] },
	{ id: "ev_noah_away", calendarId: "cal_away", title: "Noah Kim out of office", allDay: true, startDay: "2026-11-25", endDay: "2026-11-27", memberIds: ["m_noah"] },

	// From Outlook (Microsoft 365).
	{ id: "ev_o_cle", calendarId: "cal_outlook", title: "CLE webinar: generative AI and the duty of competence", start: "2026-10-22T13:00", end: "2026-10-22T14:00", location: "Online" },
	{ id: "ev_o_bar", calendarId: "cal_outlook", title: "City bar committee on technology", start: "2026-10-27T18:00", end: "2026-10-27T19:30", location: "42 West 44th Street" },
	{ id: "ev_o_onboarding", calendarId: "cal_outlook", title: "Onboarding of the new docketing rules", start: "2026-10-28T11:00", end: "2026-10-28T11:30", memberIds: ["m_evelyn", "m_iris", "m_lena"] },

	// Holidays in the United States.
	{ id: "ev_h_labor", calendarId: "cal_holidays", title: "Labor Day", allDay: true, startDay: "2026-09-07", endDay: "2026-09-07" },
	{ id: "ev_h_columbus", calendarId: "cal_holidays", title: "Columbus Day", allDay: true, startDay: "2026-10-12", endDay: "2026-10-12" },
	{ id: "ev_h_halloween", calendarId: "cal_holidays", title: "Halloween", allDay: true, startDay: "2026-10-31", endDay: "2026-10-31" },
	{ id: "ev_h_election", calendarId: "cal_holidays", title: "Election Day", allDay: true, startDay: "2026-11-03", endDay: "2026-11-03" },
	{ id: "ev_h_veterans", calendarId: "cal_holidays", title: "Veterans Day", allDay: true, startDay: "2026-11-11", endDay: "2026-11-11" },
	{ id: "ev_h_thanksgiving", calendarId: "cal_holidays", title: "Thanksgiving Day", allDay: true, startDay: "2026-11-26", endDay: "2026-11-26" },
	{ id: "ev_h_black_friday", calendarId: "cal_holidays", title: "Day after Thanksgiving", allDay: true, startDay: "2026-11-27", endDay: "2026-11-27" },
	{ id: "ev_h_christmas_eve", calendarId: "cal_holidays", title: "Christmas Eve", allDay: true, startDay: "2026-12-24", endDay: "2026-12-24" },
	{ id: "ev_h_christmas", calendarId: "cal_holidays", title: "Christmas Day", allDay: true, startDay: "2026-12-25", endDay: "2026-12-25" },
	{ id: "ev_h_new_year_eve", calendarId: "cal_holidays", title: "New Year's Eve", allDay: true, startDay: "2026-12-31", endDay: "2026-12-31" },
	{ id: "ev_h_new_year", calendarId: "cal_holidays", title: "New Year's Day", allDay: true, startDay: "2027-01-01", endDay: "2027-01-01" },
];
