import { useLayoutEffect, useRef, useState } from "react";

import { at, barsOf, clock, laidOut, minuteOf, minutesBetween, MINUTES_IN_DAY, weekdayShort } from "./dates.js";

import styles from "./Calendar.module.css";

// The days of a day, week or four-day view, hour by hour, as Google Calendar
// lays them: the days' heads, the all-day bars, and the hours with their
// events side by side where they overlap. A press on an empty hour, or a
// drag across hours, creates an event there; an event moves when dragged,
// takes a new end when its lower edge is dragged, and opens when pressed.
export const HOUR = 48;
const SNAP = 15;

export function TimeGrid({ days, instances, calendarsById, today, nowMoment, timeZone, draft, onCreate, onOpenEvent, onChange, onPickDay }) {
	const scrollRef = useRef(null);
	const columnsRef = useRef(null);
	const [drag, setDrag] = useState(null);

	useLayoutEffect(() => {
		if (scrollRef.current != null) scrollRef.current.scrollTop = 7 * HOUR - 6;
	}, []);

	const nowMinute = minuteOf(nowMoment);
	const shown = drag?.moved && drag.kind != "create" ? instances.map(item => (item.instanceId == drag.instance.instanceId ? { ...item, start: at(drag.day, drag.start), end: at(drag.day, drag.end) } : item)) : instances;
	const bars = barsOf(shown, days);
	const lanes = Math.max(1, ...bars.map(bar => bar.lane + 1));
	const columns = `repeat(${days.length}, minmax(0, 1fr))`;

	function minuteAt(clientY, rounding = Math.round) {
		const rect = columnsRef.current.getBoundingClientRect();
		const minute = rounding(((clientY - rect.top) / HOUR) * 60 / SNAP) * SNAP;
		return Math.max(0, Math.min(MINUTES_IN_DAY, minute));
	}

	function dayAt(clientX) {
		const rect = columnsRef.current.getBoundingClientRect();
		const index = Math.floor(((clientX - rect.left) / rect.width) * days.length);
		return days[Math.max(0, Math.min(days.length - 1, index))];
	}

	function pointerDown(event) {
		if (event.button != 0) return;
		const block = event.target.closest("[data-instance]");
		if (block != null) {
			const instance = shown.find(item => item.instanceId == block.dataset.instance);
			if (instance == null) return;
			const start = minuteOf(instance.start);
			const length = Math.max(SNAP, minutesBetween(instance.start, instance.end));
			setDrag({
				kind: event.target.closest("[data-resize]") != null ? "resize" : "move",
				instance,
				rect: block.getBoundingClientRect(),
				x: event.clientX,
				y: event.clientY,
				moved: false,
				offset: minuteAt(event.clientY, Math.floor) - start,
				length,
				day: instance.start.slice(0, 10),
				start,
				end: start + length,
			});
		} else {
			const minute = minuteAt(event.clientY, Math.floor);
			setDrag({ kind: "create", day: dayAt(event.clientX), from: minute, to: minute, x: event.clientX, y: event.clientY, moved: false });
		}
		event.currentTarget.setPointerCapture(event.pointerId);
	}

	function pointerMove(event) {
		if (drag == null) return;
		const moved = drag.moved || Math.abs(event.clientX - drag.x) + Math.abs(event.clientY - drag.y) > 4;
		if (drag.kind == "create") {
			setDrag({ ...drag, moved, to: minuteAt(event.clientY, Math.floor) });
		} else if (drag.kind == "move") {
			const start = Math.max(0, Math.min(MINUTES_IN_DAY - drag.length, minuteAt(event.clientY, Math.floor) - drag.offset));
			setDrag({ ...drag, moved, day: dayAt(event.clientX), start, end: start + drag.length });
		} else {
			setDrag({ ...drag, moved, end: Math.max(drag.start + SNAP, minuteAt(event.clientY)) });
		}
	}

	function pointerUp(event) {
		if (drag == null) return;
		const current = drag;
		setDrag(null);
		if (current.kind == "create") {
			const from = Math.min(current.from, current.to);
			const to = current.moved ? Math.max(current.from, current.to) + SNAP : Math.min(MINUTES_IN_DAY, from + 60);
			onCreate({ start: at(current.day, from), end: at(current.day, to) }, { left: event.clientX, right: event.clientX, top: event.clientY - 20, bottom: event.clientY + 20 });
			return;
		}
		if (!current.moved) {
			onOpenEvent(current.instance, current.rect);
			return;
		}
		onChange(current.instance, { start: at(current.day, current.start), end: at(current.day, current.end) });
	}

	const creating = drag?.kind == "create" && drag.moved ? { day: drag.day, from: Math.min(drag.from, drag.to), to: Math.max(drag.from, drag.to) + SNAP } : null;

	return (
		<div className={styles.grid}>
			<div className={styles.grid_head}>
				<span className={styles.grid_corner} />
				<div className={styles.grid_days} style={{ gridTemplateColumns: columns }}>
					{days.map(day => (
						<div key={day} className={styles.grid_day}>
							<span className={day == today ? `${styles.grid_weekday} ${styles.is_today_text}` : styles.grid_weekday}>{weekdayShort(day).toUpperCase()}</span>
							<button className={day == today ? `${styles.grid_date} ${styles.grid_date_today}` : styles.grid_date} type="button" onClick={() => onPickDay(day)} aria-label={`Open ${day}`}>
								{Number(day.slice(8))}
							</button>
						</div>
					))}
				</div>
			</div>

			<div className={styles.grid_allday}>
				<span className={styles.grid_zone}>{timeZone}</span>
				<div className={styles.grid_bars} style={{ height: lanes * 24 + 6 }}>
					{days.map((day, index) => <span key={day} className={styles.grid_bar_cell} style={{ left: `${(index / days.length) * 100}%`, width: `${100 / days.length}%` }} />)}
					{bars.map(bar => {
						const calendar = calendarsById[bar.event.calendarId];
						return (
							<button
								key={bar.event.instanceId}
								className={styles.allday_bar}
								style={{
									left: `calc(${(bar.from / days.length) * 100}% + 2px)`,
									width: `calc(${((bar.to - bar.from + 1) / days.length) * 100}% - 6px)`,
									top: bar.lane * 24 + 2,
									"--tone": tone(calendar?.color, bar.event, nowMoment),
									clipPath: cut(bar),
								}}
								type="button"
								onClick={event => onOpenEvent(bar.event, event.currentTarget.getBoundingClientRect())}
							>
								{bar.event.title}
							</button>
						);
					})}
				</div>
			</div>

			<div ref={scrollRef} className={styles.grid_scroll}>
				<div className={styles.grid_body} style={{ height: 24 * HOUR }}>
					<div className={styles.grid_hours}>
						{Array.from({ length: 23 }, (_, index) => index + 1).map(hour => (
							<span key={hour} className={styles.grid_hour} style={{ top: hour * HOUR }}>{clock(hour * 60)}</span>
						))}
					</div>
					<div
						ref={columnsRef}
						className={styles.grid_columns}
						style={{ gridTemplateColumns: columns }}
						onPointerDown={pointerDown}
						onPointerMove={pointerMove}
						onPointerUp={pointerUp}
						onPointerCancel={() => setDrag(null)}
					>
						{days.map(day => (
							<div key={day} className={styles.grid_column}>
								{laidOut(shown, day).map(piece => (
									<Block
										key={piece.event.instanceId}
										piece={piece}
										calendar={calendarsById[piece.event.calendarId]}
										nowMoment={nowMoment}
										isDragged={drag?.moved && drag.instance?.instanceId == piece.event.instanceId}
									/>
								))}
								{creating?.day == day ? <Placeholder from={creating.from} to={creating.to} color={calendarsById.cal_mine?.color} /> : null}
								{draft != null && !draft.allDay && draft.start.slice(0, 10) == day ? <Placeholder from={minuteOf(draft.start)} to={draft.end.slice(0, 10) > day ? MINUTES_IN_DAY : minuteOf(draft.end)} color={calendarsById[draft.calendarId]?.color} title={draft.title} /> : null}
								{day == today ? <span className={styles.now} style={{ top: (nowMinute / 60) * HOUR }}><span className={styles.now_dot} /></span> : null}
							</div>
						))}
					</div>
				</div>
			</div>
		</div>
	);
}

function Block({ piece, calendar, nowMoment, isDragged }) {
	const { event, start, end, column, columns, span } = piece;
	const height = Math.max(18, ((end - start) / 60) * HOUR - 2);
	// Under 40 pixels an event says its title and hour on one line; up to 56
	// its title takes one line above the hours, and two beyond.
	const short = height < 40;
	const lines = height < 56 ? 1 : 2;
	const left = (column / columns) * 100;
	const width = (span / columns) * 100;

	return (
		<button
			className={isDragged ? `${styles.event} ${styles.event_dragged}` : styles.event}
			style={{
				top: (start / 60) * HOUR + 1,
				height,
				left: `${left}%`,
				width: `calc(${width}% - ${column + span == columns ? 10 : 2}px)`,
				"--tone": tone(calendar?.color, event, nowMoment),
				zIndex: 2 + column,
			}}
			type="button"
			data-instance={event.instanceId}
			title={`${event.title}, ${clock(event.start, true)} to ${clock(event.end, true)}`}
		>
			<span className={short ? `${styles.event_title} ${styles.event_title_short}` : styles.event_title} style={short ? undefined : { WebkitLineClamp: lines }}>
				{event.title}
				{short ? <span className={styles.event_when}>, {clock(event.start, true)}</span> : null}
			</span>
			{short ? null : <span className={styles.event_time}>{clock(event.start, true)} to {clock(event.end, true)}</span>}
			{!short && height > 54 && event.location != null ? <span className={styles.event_time}>{event.location}</span> : null}
			<span className={styles.event_resize} data-resize="" />
		</button>
	);
}

function Placeholder({ from, to, color, title }) {
	const height = Math.max(18, ((to - from) / 60) * HOUR - 2);
	return (
		<span className={styles.event_placeholder} style={{ top: (from / 60) * HOUR + 1, height, "--tone": color ?? "#3f6a8a" }}>
			<span className={styles.event_title}>{title || "(No title)"}</span>
			{height >= 34 ? <span className={styles.event_time}>{clock(from, true)} to {clock(to, true)}</span> : null}
		</span>
	);
}

// An event's colour, its calendar's, made paler once it is over, as Google
// Calendar dims the past.
export function tone(color, event, nowMoment) {
	const base = event.color ?? color ?? "#3f6a8a";
	const end = event.allDay ? `${event.endDay}T24:00` : event.end;
	return end <= nowMoment ? `color-mix(in srgb, ${base} 55%, white)` : base;
}

function cut(bar) {
	if (!bar.cutBefore && !bar.cutAfter) return undefined;
	const left = bar.cutBefore ? "6px 0, 0 50%, 6px 100%" : "0 0, 0 100%";
	const right = bar.cutAfter ? "calc(100% - 6px) 100%, 100% 50%, calc(100% - 6px) 0" : "100% 100%, 100% 0";
	return `polygon(${left}, ${right})`;
}
