// The calendar's dates and times (D-076). A day is "YYYY-MM-DD" and a moment
// "YYYY-MM-DDTHH:MM", both in the firm's own time (New York in the mock), and
// all arithmetic runs in UTC so no daylight change on this computer moves
// them.

export const MINUTES_IN_DAY = 24 * 60;

export function toDate(day) {
	return new Date(`${day.slice(0, 10)}T00:00:00Z`);
}

export function dayOf(date) {
	return date.toISOString().slice(0, 10);
}

export function addDays(day, count) {
	const date = toDate(day);
	date.setUTCDate(date.getUTCDate() + count);
	return dayOf(date);
}

export function addMonths(day, count) {
	const date = toDate(day);
	const target = new Date(Date.UTC(date.getUTCFullYear(), date.getUTCMonth() + count, 1));
	const last = new Date(Date.UTC(target.getUTCFullYear(), target.getUTCMonth() + 1, 0)).getUTCDate();
	target.setUTCDate(Math.min(date.getUTCDate(), last));
	return dayOf(target);
}

export function addMinutes(moment, count) {
	const date = new Date(`${moment}:00Z`);
	date.setUTCMinutes(date.getUTCMinutes() + count);
	return date.toISOString().slice(0, 16);
}

// Minutes from midnight of the moment's own day.
export function minuteOf(moment) {
	return Number(moment.slice(11, 13)) * 60 + Number(moment.slice(14, 16));
}

export function at(day, minute) {
	const clamped = Math.max(0, Math.min(MINUTES_IN_DAY, minute));
	if (clamped == MINUTES_IN_DAY) return `${addDays(day, 1)}T00:00`;
	return `${day}T${String(Math.floor(clamped / 60)).padStart(2, "0")}:${String(clamped % 60).padStart(2, "0")}`;
}

export function minutesBetween(start, end) {
	return Math.round((new Date(`${end}:00Z`) - new Date(`${start}:00Z`)) / 60000);
}

export function weekdayOf(day) {
	return toDate(day).getUTCDay();
}

export function startOfWeek(day) {
	return addDays(day, -weekdayOf(day));
}

export function startOfMonth(day) {
	return `${day.slice(0, 7)}-01`;
}

// The days a view shows around a date.
export function daysOf(view, date) {
	if (view == "day") return [date];
	if (view == "4days") return [0, 1, 2, 3].map(index => addDays(date, index));
	if (view == "week") return [0, 1, 2, 3, 4, 5, 6].map(index => addDays(startOfWeek(date), index));
	if (view == "month") return monthGrid(date).flat();
	return [date];
}

// Six weeks from the Sunday before the month's first day, as a month view
// and the small month show them.
export function monthGrid(date) {
	const first = startOfWeek(startOfMonth(date));
	return Array.from({ length: 6 }, (_, week) => Array.from({ length: 7 }, (_, index) => addDays(first, week * 7 + index)));
}

// Where a step back or forward goes from a date, in a view.
export function stepped(view, date, direction) {
	if (view == "day") return addDays(date, direction);
	if (view == "4days") return addDays(date, 4 * direction);
	if (view == "week") return addDays(date, 7 * direction);
	if (view == "month" || view == "schedule") return addMonths(date, direction);
	if (view == "year") return addMonths(date, 12 * direction);
	return date;
}

const format = (options, day) => new Intl.DateTimeFormat("en-US", { ...options, timeZone: "UTC" }).format(toDate(day));

// The label of what a view shows: "October 2026", "Oct to Nov 2026",
// "October 23, 2026", "2026".
export function rangeLabel(view, date) {
	if (view == "day") return format({ month: "long", day: "numeric", year: "numeric" }, date);
	if (view == "year") return date.slice(0, 4);
	if (view == "month" || view == "schedule") return format({ month: "long", year: "numeric" }, date);
	const days = daysOf(view, date);
	const first = days[0];
	const last = days[days.length - 1];
	if (first.slice(0, 7) == last.slice(0, 7)) return format({ month: "long", year: "numeric" }, first);
	if (first.slice(0, 4) == last.slice(0, 4)) return `${format({ month: "short" }, first)} to ${format({ month: "short" }, last)} ${last.slice(0, 4)}`;
	return `${format({ month: "short", year: "numeric" }, first)} to ${format({ month: "short", year: "numeric" }, last)}`;
}

export function weekdayShort(day) {
	return format({ weekday: "short" }, day);
}

export function weekdayLong(day) {
	return format({ weekday: "long" }, day);
}

export function monthShort(day) {
	return format({ month: "short" }, day);
}

export function monthLong(day) {
	return format({ month: "long" }, day);
}

export function longDay(day) {
	return format({ weekday: "long", month: "long", day: "numeric" }, day);
}

// "9 AM", "12:30 PM": the clock of a moment or of minutes from midnight.
export function clock(momentOrMinute, compact = false) {
	const minute = typeof momentOrMinute == "number" ? momentOrMinute : minuteOf(momentOrMinute);
	const hours = Math.floor(minute / 60) % 24;
	const minutes = minute % 60;
	const suffix = hours < 12 ? (compact ? "am" : "AM") : (compact ? "pm" : "PM");
	const hour = hours % 12 == 0 ? 12 : hours % 12;
	const text = minutes == 0 ? `${hour}` : `${hour}:${String(minutes).padStart(2, "0")}`;
	return compact ? `${text}${suffix}` : `${text} ${suffix}`;
}

// The instances of events within the days from `first` to `last`: a weekly
// event repeats on its weekday until its last day.
export function instancesBetween(events, first, last) {
	const instances = [];
	for (const event of events) {
		if (event.repeat?.every == "week") {
			const firstDay = (event.allDay ? event.startDay : event.start).slice(0, 10);
			let offset = Math.max(0, Math.ceil((toDate(first) - toDate(firstDay)) / (7 * 86400000)));
			for (let day = addDays(firstDay, offset * 7); day <= last && day <= event.repeat.until; day = addDays(day, 7)) {
				if (event.skip?.includes(day)) continue;
				instances.push(shiftedTo(event, day));
			}
			continue;
		}
		const [from, to] = spanOf(event);
		if (from <= last && to >= first) instances.push({ ...event, instanceId: event.id });
	}
	return instances;
}

function shiftedTo(event, day) {
	if (event.allDay) {
		const length = (toDate(event.endDay) - toDate(event.startDay)) / 86400000;
		return { ...event, instanceId: `${event.id}@${day}`, startDay: day, endDay: addDays(day, length), seriesDay: day };
	}
	const length = minutesBetween(event.start, event.end);
	const start = `${day}${event.start.slice(10)}`;
	return { ...event, instanceId: `${event.id}@${day}`, start, end: addMinutes(start, length), seriesDay: day };
}

// The first and last day an event covers.
export function spanOf(event) {
	if (event.allDay) return [event.startDay, event.endDay];
	const lastMinute = addMinutes(event.end, -1);
	return [event.start.slice(0, 10), lastMinute.slice(0, 10) < event.start.slice(0, 10) ? event.start.slice(0, 10) : lastMinute.slice(0, 10)];
}

// Whether an event is laid on the day's hours, or as a bar above them: an
// all-day event, and one that runs a full day or more, is a bar.
export function isBar(event) {
	return event.allDay == true || minutesBetween(event.start, event.end) >= MINUTES_IN_DAY;
}

// The day's timed instances placed side by side where they overlap, as
// Google Calendar places them: each a column of its cluster, and as wide as
// the free columns to its right let it be.
export function laidOut(instances, day) {
	const pieces = instances
		.filter(event => !isBar(event) && event.start.slice(0, 10) <= day && event.end > `${day}T00:00`)
		.map(event => {
			const start = event.start.slice(0, 10) < day ? 0 : minuteOf(event.start);
			const end = event.end.slice(0, 10) > day ? MINUTES_IN_DAY : minuteOf(event.end);
			return { event, start, end: Math.max(end, start + 15) };
		})
		.filter(piece => piece.end > piece.start)
		.sort((first, second) => first.start - second.start || second.end - first.end);

	const placed = [];
	let cluster = [];
	let clusterEnd = -1;

	function close() {
		const columns = Math.max(1, ...cluster.map(piece => piece.column + 1));
		for (const piece of cluster) {
			let span = 1;
			while (piece.column + span < columns && !cluster.some(other => other.column == piece.column + span && other.start < piece.end && other.end > piece.start)) span += 1;
			placed.push({ ...piece, columns, span });
		}
		cluster = [];
	}

	for (const piece of pieces) {
		if (piece.start >= clusterEnd) {
			close();
			clusterEnd = -1;
		}
		const taken = cluster.filter(other => other.end > piece.start).map(other => other.column);
		let column = 0;
		while (taken.includes(column)) column += 1;
		cluster.push({ ...piece, column });
		clusterEnd = Math.max(clusterEnd, piece.end);
	}
	close();
	return placed;
}

// The bars of a run of days (a week's row, or the days of a time grid): each
// bar's first and last column, in lanes that never overlap.
export function barsOf(instances, days) {
	const first = days[0];
	const last = days[days.length - 1];
	const bars = instances
		.filter(isBar)
		.map(event => {
			const [from, to] = spanOf(event);
			return { event, from: Math.max(0, days.indexOf(from < first ? first : from)), to: days.indexOf(to > last ? last : to), cutBefore: from < first, cutAfter: to > last };
		})
		.filter(bar => bar.to >= 0)
		.sort((one, two) => one.from - two.from || (two.to - two.from) - (one.to - one.from));
	const lanes = [];
	for (const bar of bars) {
		let lane = 0;
		while (lanes[lane]?.some(other => other.from <= bar.to && other.to >= bar.from)) lane += 1;
		(lanes[lane] ??= []).push(bar);
		bar.lane = lane;
	}
	return bars;
}
