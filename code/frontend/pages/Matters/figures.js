import { useEffect, useState } from "react";

import { aiUsage, rates, timeEntries } from "../../data/billing.js";
import { hasNative, readMatterLifeUsage, realWork } from "../../data/native.js";
import { readAllTime, timeOfMatter, usageOnBill, useTime } from "../../data/time.js";
import { documents, threads } from "../../data/workspace.js";

// What a Matter holds and what its work came to, beside its name on the
// Matters page: its files, its threads, its people's billable time - in money
// where the firm keeps their rates, in hours where it does not yet - and what
// its AI colleagues' models cost, as Usage counts it. A figure of nothing is
// not shown.

// The Matters' time and model usage, read once the page opens: the time from
// each Matter's own record (data/time.js), the usage from Usage.
export function useMatterFigures(matters) {
	useTime();
	const [costs, setCosts] = useState({});
	const ids = matters.map(matter => matter.id).join(" ");
	useEffect(() => {
		if (hasNative() == false) return undefined;
		let isCurrent = true;
		readAllTime().catch(() => {});
		for (const id of ids.split(" ").filter(Boolean)) {
			readMatterLifeUsage(id).then(summary => {
				if (isCurrent) setCosts(previous => ({ ...previous, [id]: usageOnBill(summary)?.totals ?? [] }));
			}, () => {});
		}
		return () => {
			isCurrent = false;
		};
	}, [ids]);
	return matter => figuresOf(matter, costs[matter.id] ?? []);
}

// One Matter's figures, each with its sign and the words its tooltip says.
function figuresOf(matter, costs) {
	const isReal = matter.isReal == true;
	const counts = isReal ? realWork().counts?.[matter.id] ?? null : null;
	const files = isReal ? counts?.files ?? 0 : (documents[matter.id] ?? []).length;
	const threadCount = isReal ? counts?.threads ?? 0 : threads.filter(thread => thread.matterId == matter.id).length;
	const figures = [];
	if (files > 0) figures.push({ key: "files", icon: "document", text: String(files), words: files == 1 ? "1 file" : `${files} files` });
	if (threadCount > 0) figures.push({ key: "threads", icon: "messages", text: String(threadCount), words: threadCount == 1 ? "1 thread" : `${threadCount} threads` });
	const people = peopleOf(matter, isReal);
	if (people != null) figures.push({ key: "people", icon: "people", ...people });
	const ai = aiOf(matter, isReal, costs);
	if (ai != null) figures.push({ key: "ai", icon: "usage", ...ai });
	return figures;
}

// The people's billable time: its worth at their rates in the prototype's
// firm, which keeps them; its hours in the app, whose firm keeps no rates yet.
function peopleOf(matter, isReal) {
	if (isReal) {
		const hours = timeOfMatter(matter.id).filter(entry => entry.billable).reduce((sum, entry) => sum + entry.hours, 0);
		if (hours <= 0) return null;
		const text = `${hours.toFixed(1)} h`;
		return { text, words: `People's billable time: ${hours.toFixed(1)} hours` };
	}
	const amount = timeEntries.filter(entry => entry.matterId == matter.id).reduce((sum, entry) => sum + entry.hours * (rates[entry.memberId] ?? 0), 0);
	if (amount <= 0) return null;
	return { text: moneyText(amount, "USD"), words: `People's billable time: ${moneyText(amount, "USD", true)}` };
}

// What the AI colleagues' models cost on it: the largest currency shown,
// every one said.
function aiOf(matter, isReal, costs) {
	if (isReal) {
		const totals = costs.filter(total => total.micros > 0).sort((one, other) => other.micros - one.micros);
		if (totals.length == 0) return null;
		const all = totals.map(total => moneyText(total.micros / 1000000, total.currency, true)).join(" and ");
		return { text: moneyText(totals[0].micros / 1000000, totals[0].currency), words: `AI colleagues' model usage: ${all}` };
	}
	const amount = aiUsage.filter(entry => entry.matterId == matter.id).reduce((sum, entry) => sum + entry.cost, 0);
	if (amount <= 0) return null;
	return { text: moneyText(amount, "USD"), words: `AI colleagues' model usage: ${moneyText(amount, "USD", true)}` };
}

// "$4,820" and "$12.40": whole where it is a hundred or more, to the cent
// below, never a small cost read as nothing; `isExact` says the cents.
function moneyText(amount, currency, isExact = false) {
	const digits = isExact || amount < 100 ? (amount > 0 && amount < 0.01 ? 4 : 2) : 0;
	try {
		return new Intl.NumberFormat("en-US", { style: "currency", currency, minimumFractionDigits: digits, maximumFractionDigits: digits }).format(amount);
	} catch {
		return `${currency} ${amount.toFixed(digits)}`;
	}
}
