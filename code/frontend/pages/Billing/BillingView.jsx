import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { Button } from "../../components/Button/Button.jsx";
import { Chip } from "../../components/Chip/Chip.jsx";
import { Facts, Row, Rows, Tray } from "../../components/Tray/Tray.jsx";
import { agreements, billingPeriod } from "../../data/billing.js";
import { findMember, positionOf } from "../words.js";
import { longDate, money, shortDate } from "./bill.js";

import styles from "./BillingView.module.css";

// A Matter's bill for the period, for its leads (D-075): people's time at
// their rates; the AI colleagues' work on the basis the engagement letter
// agrees in advance; AI model usage as a disbursement at cost; the budget;
// and the invoice one press away.
export function MatterBill({ bill, onOpen }) {
	const share = Math.round(bill.budget.share * 100);
	const overhead = bill.matter.memberIds
		.map(id => findMember(id))
		.filter(member => member?.kind == "ai_colleague" && agreements[member.id]?.basis == "overhead");

	return (
		<div className={styles.bill}>
			<div className={styles.top}>
				<div className={styles.total}>
					<p className={styles.total_label}>To bill for {billingPeriod.label}</p>
					<p className={styles.total_amount}>{money(bill.total)}</p>
					<p className={styles.total_line}>{money(bill.fees)} in fees and {money(bill.disbursements)} in disbursements, through {longDate(billingPeriod.to).replace(/, \d{4}$/, "")}.</p>
				</div>
				<div className={styles.budget}>
					<p className={styles.total_label}>Fee budget</p>
					<p className={styles.budget_amount}>{money(bill.budget.used)} <span>of {money(bill.budget.amount)}</span></p>
					<div className={styles.meter} role="img" aria-label={`${share}% of the budget used`}>
						<span className={bill.budget.share >= 1 ? styles.meter_over : bill.budget.share >= 0.85 ? styles.meter_near : undefined} style={{ width: `${Math.min(100, share)}%` }} />
					</div>
				</div>
				<div className={styles.invoice}>
					<Button variant="primary" icon="document" onClick={() => onOpen({ kind: "invoice", matterId: bill.matter.id })}>Open the invoice</Button>
					<Chip tone="time">Draft until {bill.partner.name.split(" ")[0]} approves it</Chip>
				</div>
			</div>

			<div className={styles.columns}>
				<Tray icon="people" label="Fees" count={bill.people.length + bill.colleagues.length}>
					<Rows>
						{bill.people.map(row => (
							<Row
								key={row.member.id}
								lead={<Avatar member={row.member} size={32} />}
								title={row.member.name}
								line={`${row.quantity} ${row.quantity == 1 ? "hour" : "hours"} as ${positionOf(row.member)} at ${money(row.rate)}`}
								aside={<span className={styles.amount}>{money(row.amount)}</span>}
							/>
						))}
						{bill.colleagues.map(row => (
							<Row
								key={`${row.member.id}-${row.kind}`}
								lead={<Avatar member={row.member} size={32} />}
								title={row.member.name}
								line={row.kind == "fixed" ? `${row.quantity == 1 ? "A fixed fee" : `${row.quantity} fixed fees`} of ${money(row.rate)}, agreed in advance` : `${row.quantity} hours of measured work at ${money(row.rate)}`}
								aside={<span className={styles.amount}>{money(row.amount)}</span>}
							/>
						))}
					</Rows>
				</Tray>
				<Tray icon="document" label="Disbursements" count={bill.usage.length + bill.other.length}>
					{bill.usage.length + bill.other.length > 0 ? (
						<Rows>
							{bill.usage.map(item => (
								<Row
									key={item.id}
									lead={<Avatar member={item.member} size={32} />}
									title="AI model usage, at cost"
									line={`${item.member.name}, ${item.requests} requests, with no markup`}
									aside={<span className={styles.amount}>{money(item.amount)}</span>}
								/>
							))}
							{bill.other.map(item => (
								<Row key={item.id} title={item.text.replace(/\.$/, "")} line={shortDate(item.date)} aside={<span className={styles.amount}>{money(item.amount)}</span>} />
							))}
						</Rows>
					) : <p className={styles.empty}>No disbursements this period.</p>}
				</Tray>
				<Tray icon="info" label="On the books">
					<Facts items={[
						{ label: "Matter number", value: bill.books.number },
						{ label: "Client reference", value: bill.books.clientNumber },
						{ label: "Responsible partner", value: bill.partner.name },
						bill.previous > 0 ? { label: "Billed before", value: money(bill.previous) } : null,
						bill.paid != null ? { label: "Paid", value: `${money(bill.paid.amount)}, ${bill.paid.date}` } : null,
						bill.trust != null ? { label: "In trust", value: money(bill.trust) } : null,
					]} />
					{overhead.length > 0 ? <p className={styles.note}>{overhead.map(member => member.name).join(" and ")} {overhead.length == 1 ? "keeps" : "keep"} this Matter's deadlines on a local model; that work is the firm's own overhead and is never billed.</p> : null}
				</Tray>
			</div>
		</div>
	);
}
