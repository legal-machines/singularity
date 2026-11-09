import { agreements, billingPeriod, client, firm, invoices, matterBilling, payments, rates } from "../../data/billing.js";
import { hasNative, shownBillables } from "../../data/native.js";
import { matters } from "../../data/workspace.js";
import { findMember } from "../words.js";

// A Matter's bill for the period: people's time at their rates, the AI
// colleagues' work on the basis Schedule B agrees, and the disbursements, AI
// model usage among them at cost. Every amount is in whole cents.

const cents = amount => Math.round(amount * 100) / 100;
const sum = (lines, key = "amount") => cents(lines.reduce((total, line) => total + line[key], 0));

// LEDES timekeeper classifications (the LEDES Oversight Committee's codes).
const classifications = {
	Partner: "PARTNR",
	"Senior Counsel": "OFCOUN",
	"Of Counsel": "OFCOUN",
	Counsel: "OFCOUN",
	"Senior Associate": "ASSOC",
	Associate: "ASSOC",
	Paralegal: "PARALG",
};

export function billOf(matterId) {
	const matter = matters.find(item => item.id == matterId);
	const books = matterBilling[matterId];
	const { timeEntries, aiWork, aiUsage, expenses } = shownBillables();
	// Here nothing was billed or paid before the firm's own work began.
	const billedBefore = hasNative() ? 0 : books.billedBefore;

	const time = timeEntries
		.filter(entry => entry.matterId == matterId && entry.billable != false)
		.map(entry => {
			const member = findMember(entry.memberId);
			const rate = rates[entry.memberId];
			return { ...entry, kind: "time", member, quantity: entry.hours, rate, amount: cents(entry.hours * rate) };
		});

	const ai = aiWork
		.filter(work => work.matterId == matterId)
		.map(work => {
			const member = findMember(work.memberId);
			const agreement = agreements[work.memberId];
			if (agreement.basis == "fixed") {
				return { ...work, kind: "fixed", member, agreement, quantity: work.units, rate: agreement.fee, amount: cents(work.units * agreement.fee) };
			}
			return { ...work, kind: "measured", member, agreement, quantity: work.hours, rate: agreement.rate, amount: cents(work.hours * agreement.rate) };
		});

	const usage = aiUsage
		.filter(item => item.matterId == matterId)
		.map(item => ({ ...item, kind: "usage", member: findMember(item.memberId), code: "E124", date: item.to, amount: item.cost }));

	const other = expenses
		.filter(item => item.matterId == matterId)
		.map(item => ({ ...item, kind: "expense" }));

	const fees = cents(sum(time) + sum(ai));
	const disbursements = cents(sum(usage) + sum(other));
	const total = cents(fees + disbursements);
	const paid = hasNative() ? null : payments[matterId] ?? null;
	const previous = paid?.amount ?? 0;

	return {
		matter,
		books,
		invoice: invoices[matterId],
		partner: findMember(books.partnerId),
		time: [...time].sort(byDate),
		ai: [...ai].sort(byDate),
		usage,
		other: [...other].sort(byDate),
		people: summary(time),
		colleagues: summary(ai),
		fees,
		disbursements,
		total,
		previous,
		paid,
		due: cents(total + previous - (paid?.amount ?? 0)),
		budget: { amount: books.budget, used: cents(billedBefore + fees), share: (billedBefore + fees) / books.budget },
		trust: books.trust ?? null,
	};
}

function byDate(first, second) {
	return first.date.localeCompare(second.date);
}

// Each timekeeper's or AI colleague's hours or tasks, rate and amount.
function summary(lines) {
	const rows = [];
	for (const line of lines) {
		let row = rows.find(item => item.member.id == line.member.id && item.kind == line.kind);
		if (row == null) {
			row = { member: line.member, kind: line.kind, rate: line.rate, quantity: 0, amount: 0 };
			rows.push(row);
		}
		row.quantity = cents(row.quantity + line.quantity);
		row.amount = cents(row.amount + line.amount);
	}
	return rows.sort((first, second) => second.amount - first.amount);
}

// The open Matters that have anything to bill in the period, and the closed
// ones that still do. Whether a member leads a Matter's billing: its
// responsible partner or the lawyer who leads it.
export function leadsBilling(matterId, memberId) {
	const books = matterBilling[matterId];
	return books != null && (books.partnerId == memberId || books.leadId == memberId);
}

export function billedMatters() {
	return matters
		.filter(matter => matterBilling[matter.id] != null)
		.map(matter => billOf(matter.id))
		.filter(bill => bill.total > 0);
}

// "$1,234.50"
export function money(amount) {
	return new Intl.NumberFormat("en-US", { style: "currency", currency: "USD" }).format(amount);
}

// "1,234.50", as an invoice's columns print it.
export function amountText(amount) {
	return new Intl.NumberFormat("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 }).format(amount);
}

// "Oct 23", "October 23, 2026"
export function shortDate(iso) {
	return new Intl.DateTimeFormat("en-US", { month: "short", day: "numeric", timeZone: "UTC" }).format(new Date(`${iso.slice(0, 10)}T00:00:00Z`));
}

export function longDate(iso) {
	return new Intl.DateTimeFormat("en-US", { month: "long", day: "numeric", year: "numeric", timeZone: "UTC" }).format(new Date(`${iso.slice(0, 10)}T00:00:00Z`));
}

export function dueDate() {
	const date = new Date(`${billingPeriod.invoiceDate}T00:00:00Z`);
	date.setUTCDate(date.getUTCDate() + client.terms);
	return date.toISOString().slice(0, 10);
}

// The timekeeper's LEDES name: "Last, First"; an AI colleague by its name.
function ledesName(member) {
	const parts = member.name.split(" ");
	return parts.length > 1 ? `${parts.slice(1).join(" ")}, ${parts[0]}` : member.name;
}

function timekeeperId(member) {
	return member.kind == "ai_colleague" ? `AI-${member.name.toUpperCase()}` : member.id.replace(/^m_/, "MF-").toUpperCase();
}

const ledesDate = iso => iso.slice(0, 10).replaceAll("-", "");
const ledesAmount = amount => amount.toFixed(2);
const ledesText = text => text.replaceAll("|", "/").replaceAll("[]", "").replace(/\s+/g, " ").trim();

// The bill as a LEDES 1998B file: its header, its field names, and one line
// per item, each ending in "[]" and a carriage return and line feed.
export function ledes(bill) {
	const fields = [
		"INVOICE_DATE", "INVOICE_NUMBER", "CLIENT_ID", "LAW_FIRM_MATTER_ID", "INVOICE_TOTAL", "BILLING_START_DATE",
		"BILLING_END_DATE", "INVOICE_DESCRIPTION", "LINE_ITEM_NUMBER", "EXP/FEE/INV_ADJ_TYPE", "LINE_ITEM_NUMBER_OF_UNITS",
		"LINE_ITEM_ADJUSTMENT_AMOUNT", "LINE_ITEM_TOTAL", "LINE_ITEM_DATE", "LINE_ITEM_TASK_CODE", "LINE_ITEM_EXPENSE_CODE",
		"LINE_ITEM_ACTIVITY_CODE", "TIMEKEEPER_ID", "LINE_ITEM_DESCRIPTION", "LAW_FIRM_ID", "LINE_ITEM_UNIT_COST",
		"TIMEKEEPER_NAME", "TIMEKEEPER_CLASSIFICATION", "CLIENT_MATTER_ID",
	];
	const head = [
		ledesDate(billingPeriod.invoiceDate),
		bill.invoice.number,
		client.id,
		bill.books.number,
		ledesAmount(bill.total),
		ledesDate(billingPeriod.from),
		ledesDate(billingPeriod.to),
		ledesText(`For professional services rendered on ${bill.matter.name}`),
	];
	const lines = [];

	for (const line of bill.time) {
		lines.push([...head, String(lines.length + 1), "F", line.hours.toFixed(1), "0", ledesAmount(line.amount), ledesDate(line.date), line.task, "", line.activity, timekeeperId(line.member), ledesText(line.text), firm.taxId, ledesAmount(line.rate), ledesName(line.member), classifications[line.member.position] ?? "OT", bill.books.clientNumber]);
	}
	for (const line of bill.ai) {
		const fixed = line.kind == "fixed";
		const description = fixed
			? `${line.text} Fixed fee under Schedule B of the engagement letter of ${client.engagement}.`
			: `${line.text} ${line.hours.toFixed(1)} hours of measured work at the rate Schedule B of the engagement letter of ${client.engagement} agrees.`;
		lines.push([...head, String(lines.length + 1), "F", fixed ? String(line.units) : line.hours.toFixed(1), "0", ledesAmount(line.amount), ledesDate(line.date), line.task, "", line.activity, timekeeperId(line.member), ledesText(description), firm.taxId, ledesAmount(line.rate), ledesName(line.member), fixed ? "FLTFEE" : "OT", bill.books.clientNumber]);
	}
	for (const line of bill.usage) {
		const text = `AI model usage, at cost: ${line.member.name}, ${line.requests} requests, ${shortDate(line.from)} to ${shortDate(line.to)}.`;
		lines.push([...head, String(lines.length + 1), "E", "1", "0", ledesAmount(line.amount), ledesDate(line.to), "", line.code, "", "", ledesText(text), firm.taxId, ledesAmount(line.amount), "", "", bill.books.clientNumber]);
	}
	for (const line of bill.other) {
		lines.push([...head, String(lines.length + 1), "E", "1", "0", ledesAmount(line.amount), ledesDate(line.date), "", line.code, "", "", ledesText(line.text), firm.taxId, ledesAmount(line.amount), "", "", bill.books.clientNumber]);
	}

	return ["LEDES1998B[]", `${fields.join("|")}[]`, ...lines.map(values => `${values.join("|")}[]`)].join("\r\n") + "\r\n";
}
