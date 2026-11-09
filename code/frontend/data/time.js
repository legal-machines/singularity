import { useSyncExternalStore } from "react";

import { hasNative, nativeApp } from "./native.js";
import { matters } from "./workspace.js";

// The time members record on the firm's Matters, as the backend keeps it in
// each Matter's own data: read for a Matter when its page, the calendar or
// the time sheet asks, and again whenever the backend says the Matter
// changed, so an entry recorded on another of the firm's computers arrives as
// its threads do. Nothing is kept here as a second truth: what the window
// shows is what the backend listed last.

let state = { byMatter: {}, actorId: "", version: 0 };
const listeners = new Set();
let isListening = false;

function emit(next) {
	state = { ...state, ...next, version: state.version + 1 };
	for (const listener of listeners) listener();
}

function subscribe(listener) {
	listeners.add(listener);
	return () => listeners.delete(listener);
}

// The time as the pages read it; they render again when it changes.
export function useTime() {
	return useSyncExternalStore(subscribe, () => state);
}

function realIdOf(uiMatterId) {
	return matters.find(matter => matter.id == uiMatterId)?.realMatterId ?? null;
}

function uiIdOf(realMatterId) {
	return matters.find(matter => matter.realMatterId == realMatterId)?.id ?? `rm_${realMatterId}`;
}

// Hours in tenths, as a firm bills them: a started tenth counts.
export function tenthsOf(minutes) {
	return Math.ceil(minutes / 6) / 10;
}

// "09:30" and 90 minutes end at "11:00".
function endOf(start, minutes) {
	const [hours, mins] = start.split(":").map(Number);
	const total = hours * 60 + mins + minutes;
	return `${String(Math.floor(total / 60)).padStart(2, "0")}:${String(total % 60).padStart(2, "0")}`;
}

// An entry as the window shows it: on its Matter's page address, with its
// hours in tenths, and whether it is the member's own to change.
export function shownEntry(entry, realMatterId, actorId) {
	const start = entry.start || null;
	return {
		id: entry.id,
		matterId: uiIdOf(realMatterId),
		realMatterId,
		actorId: entry.actorId,
		memberName: entry.actorName || "A member",
		isYours: actorId != "" && entry.actorId == actorId,
		date: entry.date,
		start,
		end: start != null ? endOf(start, entry.minutes) : null,
		minutes: entry.minutes,
		hours: tenthsOf(entry.minutes),
		text: entry.text,
		billable: entry.billable != false,
		recordedAt: entry.createdAt,
	};
}

async function readTime(realMatterId) {
	const listed = await nativeApp().listTime(realMatterId);
	emit({
		byMatter: { ...state.byMatter, [realMatterId]: listed?.entries ?? [] },
		actorId: listed?.actorId || state.actorId,
	});
}

// Reads a Matter's time again when the backend says it changed, here or on
// another of the firm's computers.
function listen() {
	if (isListening == true || hasNative() == false) return;
	isListening = true;
	nativeApp().on("matter.changed", payload => {
		const changed = payload?.projectId;
		const known = Object.keys(state.byMatter).filter(id => changed == null || id == changed);
		for (const id of known) readTime(id).catch(() => {});
	});
}

// Reads the time of one Matter of the firm, by its page address.
export async function readMatterTime(uiMatterId) {
	if (hasNative() == false) return;
	const realMatterId = realIdOf(uiMatterId);
	if (realMatterId == null) return;
	listen();
	await readTime(realMatterId);
}

// Reads the time of every Matter of the firm known here: the calendar and
// the time sheet show the member's own across them.
export async function readAllTime() {
	if (hasNative() == false) return;
	listen();
	const ids = matters.filter(matter => matter.isReal == true && matter.realMatterId != null).map(matter => matter.realMatterId);
	await Promise.all(ids.map(id => readTime(id).catch(() => {})));
}

// A Matter's time, every member's, the latest day first.
export function timeOfMatter(uiMatterId) {
	const realMatterId = realIdOf(uiMatterId);
	if (realMatterId == null) return [];
	return (state.byMatter[realMatterId] ?? []).map(entry => shownEntry(entry, realMatterId, state.actorId));
}

// Whether a Matter's time has been read yet: before it, the window says it
// is reading, never that nothing is recorded.
export function isMatterTimeRead(uiMatterId) {
	const realMatterId = realIdOf(uiMatterId);
	return realMatterId != null && state.byMatter[realMatterId] != null;
}

// The member's own time across the firm's Matters read here.
export function ownTime() {
	return Object.entries(state.byMatter)
		.flatMap(([realMatterId, entries]) => entries.map(entry => shownEntry(entry, realMatterId, state.actorId)))
		.filter(entry => entry.isYours == true);
}

function inputOf({ date, start, minutes, text, billable }) {
	const input = {};
	if (date !== undefined) input.date = date;
	if (start !== undefined) input.start = start ?? "";
	if (minutes !== undefined) input.minutes = Math.round(minutes);
	if (text !== undefined) input.text = text;
	if (billable !== undefined) input.billable = billable;
	return input;
}

// Records the member's time on a Matter. The backend refuses, in its own
// words, what a bill could not read.
export async function recordTime(uiMatterId, entry) {
	const realMatterId = realIdOf(uiMatterId);
	if (realMatterId == null) throw new Error("Choose the Matter the time was worked on.");
	listen();
	const recorded = await nativeApp().recordTime(realMatterId, inputOf(entry));
	await readTime(realMatterId).catch(() => {});
	return recorded;
}

export async function changeTime(uiMatterId, entryId, change) {
	const realMatterId = realIdOf(uiMatterId);
	if (realMatterId == null) throw new Error("That Matter is not open on this computer.");
	const changed = await nativeApp().updateTime(realMatterId, entryId, inputOf(change));
	await readTime(realMatterId).catch(() => {});
	return changed;
}

// Moves the member's own entry to another Matter: recorded there, and taken
// back from where it was.
export async function moveTime(fromUiMatterId, entryId, toUiMatterId, entry) {
	const recorded = await recordTime(toUiMatterId, entry);
	await takeBackTime(fromUiMatterId, entryId);
	return recorded;
}

export async function takeBackTime(uiMatterId, entryId) {
	const realMatterId = realIdOf(uiMatterId);
	if (realMatterId == null) throw new Error("That Matter is not open on this computer.");
	await nativeApp().removeTime(realMatterId, entryId);
	await readTime(realMatterId).catch(() => {});
}

// The words of a refusal as the member reads them.
export function problemOf(error, fallback) {
	return error?.userMessage || error?.message || fallback;
}

// ---- Months -------------------------------------------------------------------------

// "October 2026"
export function monthText(month) {
	const [year, number] = month.split("-").map(Number);
	return new Intl.DateTimeFormat("en-US", { month: "long", year: "numeric", timeZone: "UTC" }).format(new Date(Date.UTC(year, number - 1, 1)));
}

// The month a step before or after: "2026-01" a step back is "2025-12".
export function steppedMonth(month, step) {
	const [year, number] = month.split("-").map(Number);
	return new Date(Date.UTC(year, number - 1 + step, 1)).toISOString().slice(0, 7);
}

// ---- The AI colleagues' work on the bill -------------------------------------------

// A month as Usage reads a period: from the first day's midnight here to the
// next month's, in UTC to the second ("2026-09-30T21:00:00Z").
export function monthPeriod(month) {
	const [year, number] = month.split("-").map(Number);
	const utc = date => date.toISOString().replace(/\.\d{3}Z$/, "Z");
	return { start: utc(new Date(year, number - 1, 1)), end: utc(new Date(year, number, 1)) };
}

// The AI work on a Matter in a month, as Usage counts it (usage.getSummary)
// from the Matter's own record of each request, which travels with the Matter
// to every computer of the firm: whichever computer ran a request, it is on
// the bill. Never estimated here.
export async function readMatterUsage(uiMatterId, month) {
	const realMatterId = realIdOf(uiMatterId);
	if (hasNative() == false || realMatterId == null) return null;
	const period = monthPeriod(month);
	return nativeApp().getUsageSummary(period.start, period.end, realMatterId);
}

// The month's AI work as a statement for the Matter's client: a PDF to send
// with the invoice and its signed file anyone can check, written and signed
// by this computer into the folder the lawyer chooses. Null when they chose
// none.
export function canSaveWorkStatement() {
	return hasNative() && typeof nativeApp()?.saveWorkStatement == "function";
}

export async function saveWorkStatement(uiMatterId, month) {
	const realMatterId = realIdOf(uiMatterId);
	if (hasNative() == false || realMatterId == null) throw new Error("This Matter's statement is made on a computer that holds it.");
	const period = monthPeriod(month);
	// Each OpenRouter cost confirmed by its own record, and this computer's
	// record timestamped, before the statement is signed.
	await confirmWorkCosts(uiMatterId, month).catch(() => null);
	const saved = await nativeApp().saveWorkStatement(realMatterId, period.start, period.end, monthText(month));
	return saved?.saved == true ? saved : null;
}

// Asks OpenRouter for its own record of each of the month's OpenRouter calls
// on the Matter not confirmed yet, and Legal Machines to timestamp this
// computer's work record: { confirmed, waiting, stamped }, or null where
// this build cannot. Once a minute for a Matter and month at most, unless
// `isAgain`.
const confirmedAt = new Map();
export async function confirmWorkCosts(uiMatterId, month, isAgain = true) {
	const realMatterId = realIdOf(uiMatterId);
	if (hasNative() == false || realMatterId == null || typeof nativeApp()?.confirmWorkCosts != "function") return null;
	const key = `${realMatterId}\n${month}`;
	if (isAgain == false && Date.now() - (confirmedAt.get(key) ?? 0) < 60000) return null;
	confirmedAt.set(key, Date.now());
	const period = monthPeriod(month);
	return nativeApp().confirmWorkCosts(realMatterId, period.start, period.end);
}

// What Usage counted, as a bill reads it: the requests, the tokens, the cost
// in each currency as the providers charged or estimated it, how many requests
// have no cost recorded, and the requests that reached no model (a colleague
// asked while no model account was connected), so the bill lists every request
// it counts; and the requests by whose work they were, the reader's own marked
// by `actorId`.
export function usageOnBill(summary, actorId = state.actorId) {
	if (summary == null) return null;
	const counts = summary.classificationCounts ?? {};
	const totals = (summary.currencyTotals ?? []).map(total => ({
		currency: total.currency,
		micros: (total.actualMicros ?? 0) + (total.estimatedMicros ?? 0),
		isEstimated: (total.estimatedMicros ?? 0) > 0,
	})).filter(total => total.micros > 0);
	const models = (summary.breakdowns?.models ?? []).map(model => ({
		id: model.id,
		requests: model.operationCount ?? 0,
		tokens: model.tokens?.total ?? 0,
		totals: (model.currencyTotals ?? []).map(total => ({ currency: total.currency, micros: (total.actualMicros ?? 0) + (total.estimatedMicros ?? 0) }))
			.filter(total => total.micros > 0),
	})).sort((one, other) => other.requests - one.requests);
	const requests = summary.operationCount ?? 0;
	const tokens = summary.tokens?.total ?? 0;
	const unmodelled = {
		requests: Math.max(0, requests - models.reduce((sum, model) => sum + model.requests, 0)),
		tokens: Math.max(0, tokens - models.reduce((sum, model) => sum + model.tokens, 0)),
	};
	return {
		requests,
		tokens,
		totals,
		models,
		unmodelled,
		withoutCost: (counts.usageOnly ?? 0) + (counts.unavailable ?? 0),
		workers: workersOf(summary.operations ?? [], actorId),
	};
}

// One request as its bill line reads it: when it ran, what for, on which
// model, who asked or that the colleague began it itself, its cost as the
// provider reported or estimated it, and each model call's evidence.
function requestOnBill(operation) {
	const usage = operation.usage ?? {};
	const isPriced = operation.costClassification == "actual" || operation.costClassification == "estimated";
	return {
		id: operation.operationId,
		time: operation.time ?? "",
		operationType: operation.operationType ?? "",
		operationRole: operation.operationRole ?? "",
		chatId: operation.chatId ?? null,
		providerId: operation.providerId ?? null,
		modelId: operation.modelId ?? null,
		executionKind: operation.executionKind ?? "",
		isFailed: operation.operationStatus == "error",
		agentInitiated: operation.agentInitiated == true,
		askedBy: operation.askedBy ?? null,
		tokens: usage.totalTokens ?? 0,
		cost: isPriced && usage.costMicros != null && usage.costCurrency != null
			? { micros: usage.costMicros, currency: usage.costCurrency, isEstimated: operation.costClassification == "estimated" }
			: null,
		calls: operation.calls ?? [],
		callsRecorded: operation.callsRecorded == true,
	};
}

// The requests by whose work they were: each AI colleague's, each member's
// own, and those Singularity made to keep the Matter's context; the most
// costly first, each one's requests the latest first.
function workersOf(operations, actorId) {
	const groups = new Map();
	for (const operation of operations) {
		const agentId = operation.agentId ?? null;
		const asker = operation.askedBy ?? null;
		const key = agentId != null ? `agent:${agentId}` : asker != null ? `person:${asker.actorId}` : "upkeep";
		if (groups.has(key) == false) {
			groups.set(key, {
				key,
				kind: agentId != null ? "colleague" : asker != null ? "person" : "upkeep",
				agentId,
				actorId: agentId == null ? asker?.actorId ?? null : null,
				name: agentId != null ? operation.agentName ?? "" : asker?.name ?? "",
				isYours: agentId == null && asker != null && actorId != "" && asker.actorId == actorId,
				requests: [],
			});
		}
		const group = groups.get(key);
		if (group.name == "" && agentId != null && (operation.agentName ?? "") != "") group.name = operation.agentName;
		group.requests.push({ ...requestOnBill(operation), isAskerYou: asker != null && actorId != "" && asker.actorId == actorId });
	}
	const workers = [...groups.values()].map(worker => ({
		...worker,
		requests: worker.requests.sort((one, other) => other.time.localeCompare(one.time)),
		totals: totalsOfRequests(worker.requests),
	}));
	const spent = worker => worker.totals.reduce((sum, total) => sum + total.micros, 0);
	return workers.sort((one, other) => spent(other) - spent(one) || other.requests.length - one.requests.length);
}

// What some requests cost, in each currency, estimated where any of it is.
function totalsOfRequests(requests) {
	const totals = new Map();
	for (const request of requests) {
		if (request.cost == null || request.cost.micros <= 0) continue;
		const total = totals.get(request.cost.currency) ?? { currency: request.cost.currency, micros: 0, isEstimated: false };
		total.micros += request.cost.micros;
		total.isEstimated = total.isEstimated || request.cost.isEstimated;
		totals.set(request.cost.currency, total);
	}
	return [...totals.values()].sort((one, other) => one.currency.localeCompare(other.currency));
}

// "$0.42" from micros of a currency: to the cent, or to four places where
// the whole is less than a cent, so a small cost never reads as nothing.
export function costText(micros, currency) {
	const amount = micros / 1000000;
	const digits = amount > 0 && amount < 0.01 ? 4 : 2;
	try {
		return new Intl.NumberFormat("en-US", { style: "currency", currency, minimumFractionDigits: digits, maximumFractionDigits: digits }).format(amount);
	} catch {
		return `${currency} ${amount.toFixed(digits)}`;
	}
}
