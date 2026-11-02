import { Icon } from "../../components/Icon/Icon.jsx";
import { Page } from "../../components/Page/Page.jsx";
import { SheetBar } from "../../components/SheetBar/SheetBar.jsx";
import { agreements, billingPeriod, client, firm } from "../../data/billing.js";
import { amountText, billOf, dueDate, ledes, longDate, money, shortDate } from "../Billing/bill.js";
import { positionOf } from "../words.js";

import styles from "./InvoicePage.module.css";

// A Matter's invoice (D-076), in the form a US client's legal department
// expects: the firm's letterhead with its tax ID; the invoice's number, date,
// period and due date; the client and the Matter with both its numbers and
// its responsible partner; the fees by date with their UTBMS codes, the AI
// colleagues' work on the basis Schedule B of the engagement letter agrees;
// a summary by timekeeper; the disbursements, AI model usage at cost among
// them; the totals and the balance; the trust account; the terms. It prints
// on Letter paper as it stands, and it downloads as a LEDES 1998B file for
// the client's e-billing. Above it, never printed, the way back and what the
// draft waits for.
export function InvoicePage({ matterId, onOpen }) {
	const bill = billOf(matterId);
	const fileName = `${bill.invoice.number}.txt`;

	function download() {
		const blob = new Blob([ledes(bill)], { type: "text/plain" });
		const link = document.createElement("a");
		link.href = URL.createObjectURL(blob);
		link.download = fileName;
		link.click();
		URL.revokeObjectURL(link.href);
	}

	return (
		<Page bar={<SheetBar trail={[{ label: bill.matter.name, place: { kind: "matter", matterId, tab: "overview" } }, { label: "Billing", place: { kind: "matter", matterId, tab: "billing" } }, { label: `Invoice ${bill.invoice.number}` }]} />}>
			<div className={styles.chrome} data-print="hide">
				<button className={styles.back} type="button" onClick={() => onOpen({ kind: "matter", matterId, tab: "billing" })}>
					<Icon name="back" />
					{bill.matter.name}
				</button>
				<div className={styles.chrome_row}>
					<div>
						<h1 className={styles.title}>Invoice {bill.invoice.number}</h1>
						<p className={styles.state}>
							A draft for {bill.partner.name}'s approval before it goes to {client.name.replace(/, Inc\.$/, "")}.
						</p>
					</div>
					<div className={styles.actions}>
						<button className={styles.secondary} type="button" onClick={download} title={`Saves ${fileName}`}>Download LEDES 1998B</button>
						<button className={styles.primary} type="button" onClick={() => window.print()}>Print</button>
					</div>
				</div>
			</div>

			<article className={styles.sheet} aria-label={`Invoice ${bill.invoice.number}`}>
				<header className={styles.letterhead}>
					<div>
						<p className={styles.firm}>{firm.name}</p>
						<p className={styles.small}>{firm.street}</p>
						<p className={styles.small}>{firm.city}</p>
						<p className={styles.small}>{firm.phone}, {firm.email}</p>
						<p className={styles.small}>Federal tax ID {firm.taxId}</p>
					</div>
					<div className={styles.invoice_facts}>
						<p className={styles.doc_title}>Invoice</p>
						<dl className={styles.facts}>
							<dt>Invoice number</dt><dd>{bill.invoice.number}</dd>
							<dt>Invoice date</dt><dd>{longDate(billingPeriod.invoiceDate)}</dd>
							<dt>Billing period</dt><dd>{shortDate(billingPeriod.from)} to {longDate(billingPeriod.to)}</dd>
							<dt>Payment due</dt><dd>{longDate(dueDate())}</dd>
						</dl>
					</div>
				</header>

				<section className={styles.parties}>
					<div>
						<p className={styles.label}>Bill to</p>
						<p className={styles.party}>{client.name}</p>
						<p className={styles.small}>Attention: {client.attention}</p>
						<p className={styles.small}>{client.street}</p>
						<p className={styles.small}>{client.city}</p>
					</div>
					<div>
						<p className={styles.label}>Matter</p>
						<p className={styles.party}>{bill.matter.name}</p>
						<p className={styles.small}>Firm matter {bill.books.number}</p>
						<p className={styles.small}>Client matter {bill.books.clientNumber}</p>
						<p className={styles.small}>Responsible partner: {bill.partner.name}</p>
					</div>
				</section>

				<p className={styles.lead}>For professional services rendered through {longDate(billingPeriod.to)}.</p>

				<h2 className={styles.section}>Fees</h2>
				<table className={styles.table}>
					<thead>
						<tr>
							<th className={styles.col_date}>Date</th>
							<th className={styles.col_who}>Timekeeper</th>
							<th className={styles.col_code}>Task</th>
							<th className={styles.col_code}>Activity</th>
							<th>Description</th>
							<th className={styles.col_number}>Hours</th>
							<th className={styles.col_number}>Rate</th>
							<th className={styles.col_number}>Amount</th>
						</tr>
					</thead>
					<tbody>
						{bill.time.map(line => (
							<tr key={line.id}>
								<td>{shortDate(line.date)}</td>
								<td>{line.member.name}</td>
								<td>{line.task}</td>
								<td>{line.activity}</td>
								<td>{line.text}</td>
								<td className={styles.number}>{line.hours.toFixed(1)}</td>
								<td className={styles.number}>{amountText(line.rate)}</td>
								<td className={styles.number}>{amountText(line.amount)}</td>
							</tr>
						))}
					</tbody>
				</table>

				{
					bill.ai.length > 0
					?
						<>
							<h3 className={styles.subsection}>AI colleagues' work, under Schedule B of the engagement letter of {client.engagement}</h3>
							<table className={styles.table}>
								<tbody>
									{bill.ai.map(line => (
										<tr key={line.id}>
											<td className={styles.col_date}>{shortDate(line.date)}</td>
											<td className={styles.col_who}>{line.member.name}</td>
											<td className={styles.col_code}>{line.task}</td>
											<td className={styles.col_code}>{line.activity}</td>
											<td>
												{line.text}
												<span className={styles.basis}>{line.kind == "fixed" ? ` A fixed fee for ${agreements[line.member.id].task.replace(/^[A-Z](?=[a-z])/, letter => letter.toLowerCase())}.` : ` Measured work, at the agreed rate.`}</span>
											</td>
											<td className={`${styles.number} ${styles.col_number}`}>{line.kind == "fixed" ? "Fixed" : line.hours.toFixed(1)}</td>
											<td className={`${styles.number} ${styles.col_number}`}>{amountText(line.rate)}</td>
											<td className={`${styles.number} ${styles.col_number}`}>{amountText(line.amount)}</td>
										</tr>
									))}
								</tbody>
							</table>
						</>
					:
						null
				}

				<div className={styles.subtotal}>
					<span>Total fees</span>
					<span className={styles.number}>{amountText(bill.fees)}</span>
				</div>

				<h2 className={styles.section}>Timekeeper summary</h2>
				<table className={styles.table}>
					<thead>
						<tr>
							<th>Name</th>
							<th>Classification</th>
							<th className={styles.col_number}>Hours</th>
							<th className={styles.col_number}>Rate</th>
							<th className={styles.col_number}>Amount</th>
						</tr>
					</thead>
					<tbody>
						{bill.people.map(row => (
							<tr key={row.member.id}>
								<td>{row.member.name}</td>
								<td>{positionOf(row.member)}</td>
								<td className={styles.number}>{row.quantity.toFixed(1)}</td>
								<td className={styles.number}>{amountText(row.rate)}</td>
								<td className={styles.number}>{amountText(row.amount)}</td>
							</tr>
						))}
						{bill.colleagues.map(row => (
							<tr key={`${row.member.id}-${row.kind}`}>
								<td>{row.member.name}</td>
								<td>AI colleague, {row.kind == "fixed" ? "fixed fee" : "measured work"}</td>
								<td className={styles.number}>{row.kind == "fixed" ? `${row.quantity} ${row.quantity == 1 ? "task" : "tasks"}` : row.quantity.toFixed(1)}</td>
								<td className={styles.number}>{amountText(row.rate)}</td>
								<td className={styles.number}>{amountText(row.amount)}</td>
							</tr>
						))}
					</tbody>
				</table>

				{
					bill.usage.length + bill.other.length > 0
					?
						<>
							<h2 className={styles.section}>Disbursements</h2>
							<table className={styles.table}>
								<thead>
									<tr>
										<th className={styles.col_date}>Date</th>
										<th className={styles.col_code}>Code</th>
										<th>Description</th>
										<th className={styles.col_number}>Amount</th>
									</tr>
								</thead>
								<tbody>
									{bill.usage.map(item => (
										<tr key={item.id}>
											<td>{shortDate(item.to)}</td>
											<td>{item.code}</td>
											<td>AI model usage, at cost: {item.member.name}, {item.requests} requests, {shortDate(item.from)} to {shortDate(item.to)}. Billed at the provider's actual charge, with no markup.</td>
											<td className={styles.number}>{amountText(item.amount)}</td>
										</tr>
									))}
									{bill.other.map(item => (
										<tr key={item.id}>
											<td>{shortDate(item.date)}</td>
											<td>{item.code}</td>
											<td>{item.text}</td>
											<td className={styles.number}>{amountText(item.amount)}</td>
										</tr>
									))}
								</tbody>
							</table>
							<div className={styles.subtotal}>
								<span>Total disbursements</span>
								<span className={styles.number}>{amountText(bill.disbursements)}</span>
							</div>
						</>
					:
						null
				}

				<section className={styles.totals}>
					<dl>
						<dt>Fees for this period</dt><dd>{amountText(bill.fees)}</dd>
						<dt>Disbursements for this period</dt><dd>{amountText(bill.disbursements)}</dd>
						<dt className={styles.strong}>Total for this invoice</dt><dd className={styles.strong}>{amountText(bill.total)}</dd>
						<dt>Previous balance</dt><dd>{amountText(bill.previous)}</dd>
						<dt>{bill.paid != null ? `Payment received, ${bill.paid.date}, invoice ${bill.paid.invoice}` : "Payments received"}</dt><dd>{bill.paid != null ? `(${amountText(bill.paid.amount)})` : amountText(0)}</dd>
						<dt className={styles.due}>Balance due</dt><dd className={styles.due}>{money(bill.due)}</dd>
					</dl>
				</section>

				{
					bill.trust != null
					?
						<section className={styles.trust}>
							<h2 className={styles.section}>Trust account</h2>
							<p className={styles.text}>
								Retainer held in trust for this Matter: {money(bill.trust)}. Applied to this invoice: none. It is applied only with your written consent or at the close of the Matter, under the engagement letter.
							</p>
						</section>
					:
						null
				}

				<section className={styles.terms}>
					<h2 className={styles.section}>Terms</h2>
					<p className={styles.text}>
						Payment is due within {client.terms} days of the invoice date, under the engagement letter of {client.engagement}. Please quote invoice {bill.invoice.number} with your payment. Pay by check to {firm.name}, or by ACH or wire; instructions are sent on request and confirmed by telephone.
					</p>
					<p className={styles.text}>
						AI colleagues' work is billed on the basis Schedule B of the engagement letter agrees in advance: a fixed fee for a defined task, or an agreed rate for measured work. No AI colleague's work is billed as hours set in advance. AI model usage on this Matter is passed through at the provider's actual cost, with no markup; work on the firm's local models is its own overhead and is not billed.
					</p>
				</section>
			</article>
		</Page>
	);
}
