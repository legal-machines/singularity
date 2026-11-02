import { useLayoutEffect, useRef, useState } from "react";

import { Icon } from "../../components/Icon/Icon.jsx";
import { matters } from "../../data/workspace.js";
import { addMonths, clock, isBar, monthGrid, monthLong, monthShort, spanOf, weekdayOf, weekdayShort } from "./dates.js";
import { tone } from "./TimeGrid.jsx";

import styles from "./Calendar.module.css";

const LANE = 24;

// The month (D-076): six weeks, each day with its events in lanes; a bar runs
// across the days it covers, a timed event is its dot, its hour and its
// title; what does not fit is "N more".
export function MonthView({ date, weekends, instances, calendarsById, today, nowMoment, onPickDay, onCreateDay, onOpenEvent, onMore }) {
	const bodyRef = useRef(null);
	const [rowHeight, setRowHeight] = useState(110);
	const weeks = monthGrid(date).map(week => week.filter(day => weekends || ![0, 6].includes(weekdayOf(day))));
	const month = date.slice(0, 7);

	useLayoutEffect(() => {
		const body = bodyRef.current;
		const measure = () => setRowHeight(body.clientHeight / 6);
		const observer = new ResizeObserver(measure);
		observer.observe(body);
		measure();
		return () => observer.disconnect();
	}, []);

	const capacity = Math.max(1, Math.floor((rowHeight - 30) / LANE));
	const columns = `repeat(${weeks[0].length}, minmax(0, 1fr))`;

	return (
		<div className={styles.month}>
			<div className={styles.month_head} style={{ gridTemplateColumns: columns }}>
				{weeks[0].map(day => <span key={day} className={weekdayOf(day) == 0 ? styles.sunday : undefined}>{weekdayShort(day).toUpperCase()}</span>)}
			</div>
			<div ref={bodyRef} className={styles.month_body}>
				{weeks.map(week => (
					<WeekRow
						key={week[0]}
						week={week}
						month={month}
						columns={columns}
						capacity={capacity}
						instances={instances}
						calendarsById={calendarsById}
						today={today}
						nowMoment={nowMoment}
						onPickDay={onPickDay}
						onCreateDay={onCreateDay}
						onOpenEvent={onOpenEvent}
						onMore={onMore}
					/>
				))}
			</div>
		</div>
	);
}

function WeekRow({ week, month, columns, capacity, instances, calendarsById, today, nowMoment, onPickDay, onCreateDay, onOpenEvent, onMore }) {
	const first = week[0];
	const last = week[week.length - 1];

	// Every event of the week in lanes: bars first, longest first, then the
	// timed events by their hour.
	const items = [];
	for (const event of instances) {
		const [from, to] = spanOf(event);
		if (to < first || from > last) continue;
		if (isBar(event)) {
			const start = week.findIndex(day => day >= from);
			const end = week.length - 1 - [...week].reverse().findIndex(day => day <= to);
			if (start < 0 || end < start) continue;
			items.push({ event, from: start, to: end, bar: true, cutBefore: from < week[start], cutAfter: to > week[end] });
		} else {
			const index = week.indexOf(event.start.slice(0, 10));
			if (index >= 0) items.push({ event, from: index, to: index, bar: false });
		}
	}
	items.sort((one, two) => (one.bar == two.bar ? 0 : one.bar ? -1 : 1) || one.from - two.from || (two.to - two.from) - (one.to - one.from) || (one.event.start ?? "").localeCompare(two.event.start ?? ""));
	const lanes = [];
	for (const item of items) {
		let lane = 0;
		while (lanes[lane]?.some(other => other.from <= item.to && other.to >= item.from)) lane += 1;
		(lanes[lane] ??= []).push(item);
		item.lane = lane;
	}

	// A day that holds more than fits keeps its last lane for "N more".
	const crowded = week.map((_, index) => items.filter(item => item.from <= index && item.to >= index).length > capacity);
	const shown = item => item.lane < capacity - 1 || (item.lane == capacity - 1 && !crowded.slice(item.from, item.to + 1).some(Boolean));
	const hiddenOn = index => items.filter(item => item.from <= index && item.to >= index && !shown(item)).length;

	return (
		<div className={styles.month_row} style={{ gridTemplateColumns: columns }}>
			{week.map((day, index) => (
				<div
					key={day}
					className={[styles.month_day, day.slice(0, 7) != month ? styles.month_day_out : "", weekdayOf(day) == 0 ? styles.sunday : ""].join(" ")}
					style={{ gridColumn: index + 1 }}
					onClick={event => {
						if (event.target == event.currentTarget) onCreateDay(day, event.currentTarget.getBoundingClientRect());
					}}
				>
					<button className={day == today ? `${styles.month_date} ${styles.month_date_today}` : styles.month_date} type="button" onClick={() => onPickDay(day)}>
						{day.slice(8) == "01" ? `${monthShort(day)} 1` : Number(day.slice(8))}
					</button>
				</div>
			))}
			<div className={styles.month_items} style={{ gridTemplateColumns: columns }}>
				{items.filter(shown).map(item => {
					const calendar = calendarsById[item.event.calendarId];
					const matter = matters.find(entry => entry.id == item.event.matterId);
					return (
						<button
							key={`${item.event.instanceId}-${item.from}`}
							className={item.bar ? `${styles.mev} ${styles.mev_whole}` : styles.mev}
							style={{ gridColumn: `${item.from + 1} / ${item.to + 2}`, gridRow: item.lane + 1, "--tone": tone(calendar?.color, item.event, nowMoment) }}
							type="button"
							title={matter != null ? `${item.event.title}, ${matter.name}` : item.event.title}
							onClick={event => onOpenEvent(item.event, event.currentTarget.getBoundingClientRect())}
						>
							{item.bar ? null : <span className={styles.mev_dot} />}
							{item.bar ? null : <span className={styles.mev_time}>{clock(item.event.start, true)}</span>}
							<span className={styles.mev_title}>{item.event.title}</span>
						</button>
					);
				})}
				{week.map((day, index) => {
					const hidden = hiddenOn(index);
					if (hidden == 0) return null;
					return (
						<button key={`more-${day}`} className={styles.more} style={{ gridColumn: index + 1, gridRow: capacity }} type="button" onClick={event => onMore(day, event.currentTarget.getBoundingClientRect())}>
							{hidden} more
						</button>
					);
				})}
			</div>
		</div>
	);
}

// A small month: the side panel's, and each month of the year's.
export function MiniMonth({ month, selected = [], today, onPick, large }) {
	const [shown, setShown] = useState(month.slice(0, 7));
	const [lastMonth, setLastMonth] = useState(month.slice(0, 7));
	if (month.slice(0, 7) != lastMonth) {
		setLastMonth(month.slice(0, 7));
		setShown(month.slice(0, 7));
	}
	const anchor = `${shown}-01`;

	return (
		<div className={large ? `${styles.mini} ${styles.mini_large}` : styles.mini}>
			<div className={styles.mini_head}>
				<span className={styles.mini_title}>{monthLong(anchor)}{large ? "" : ` ${anchor.slice(0, 4)}`}</span>
				{
					large
					?
						null
					:
						<span className={styles.mini_steps}>
							<button className={styles.icon_button_small} type="button" aria-label="Previous month" onClick={() => setShown(addMonths(anchor, -1).slice(0, 7))}><Icon name="back" size={18} /></button>
							<button className={`${styles.icon_button_small} ${styles.flip}`} type="button" aria-label="Next month" onClick={() => setShown(addMonths(anchor, 1).slice(0, 7))}><Icon name="back" size={18} /></button>
						</span>
				}
			</div>
			<div className={styles.mini_grid}>
				{["S", "M", "T", "W", "T", "F", "S"].map((letter, index) => <span key={index} className={index == 0 ? `${styles.mini_dow} ${styles.sunday}` : styles.mini_dow}>{letter}</span>)}
				{monthGrid(anchor).flat().map(day => (
					<button
						key={day}
						className={[
							styles.mini_day,
							day.slice(0, 7) != shown ? styles.mini_out : "",
							selected.includes(day) ? styles.mini_on : "",
							day == today ? styles.mini_today : "",
							weekdayOf(day) == 0 ? styles.sunday : "",
						].join(" ")}
						type="button"
						onClick={() => onPick(day)}
					>
						{Number(day.slice(8))}
					</button>
				))}
			</div>
		</div>
	);
}

// The year: its twelve months; a day opens the day.
export function YearView({ date, today, onPick }) {
	const year = date.slice(0, 4);
	return (
		<div className={styles.year}>
			{Array.from({ length: 12 }, (_, index) => `${year}-${String(index + 1).padStart(2, "0")}-01`).map(month => (
				<MiniMonth key={month} month={month} today={today} onPick={onPick} large={true} />
			))}
		</div>
	);
}

// The schedule: the days ahead that hold something, each with its events.
export function ScheduleView({ instances, calendarsById, today, nowMoment, onOpenEvent, empty }) {
	const byDay = new Map();
	for (const event of instances) {
		const day = isBar(event) ? spanOf(event)[0] : event.start.slice(0, 10);
		if (!byDay.has(day)) byDay.set(day, []);
		byDay.get(day).push(event);
	}
	const days = [...byDay.keys()].sort();

	if (days.length == 0) return <div className={styles.schedule}><p className={styles.empty}>{empty}</p></div>;

	return (
		<div className={styles.schedule}>
			{days.map(day => (
				<div key={day} className={[styles.sday, day == today ? styles.sday_today : "", weekdayOf(day) == 0 ? styles.sunday : ""].join(" ")}>
					<div className={styles.sdate}>
						<b>{Number(day.slice(8))}</b>
						<span>{monthShort(day).toUpperCase()}, {weekdayShort(day).toUpperCase()}</span>
					</div>
					<ul>
						{byDay.get(day).sort((one, two) => (isBar(one) ? "" : one.start).localeCompare(isBar(two) ? "" : two.start)).map(event => {
							const calendar = calendarsById[event.calendarId];
							const matter = matters.find(entry => entry.id == event.matterId);
							const [from, to] = spanOf(event);
							return (
								<li key={event.instanceId}>
									<button className={styles.srow} type="button" onClick={click => onOpenEvent(event, click.currentTarget.getBoundingClientRect())}>
										<span className={styles.srow_dot} style={{ background: tone(calendar?.color, event, nowMoment) }} />
										<span className={styles.srow_when}>{isBar(event) ? (from == to ? "All day" : `${monthShort(from)} ${Number(from.slice(8))} to ${monthShort(to)} ${Number(to.slice(8))}`) : `${clock(event.start, true)} to ${clock(event.end, true)}`}</span>
										<span className={styles.srow_title}>{event.title}</span>
										<span className={styles.srow_where}>{matter?.name ?? event.location ?? ""}</span>
									</button>
								</li>
							);
						})}
					</ul>
				</div>
			))}
		</div>
	);
}

