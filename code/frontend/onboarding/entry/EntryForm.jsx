import { Children, useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";
import { BackButton } from "../BackButton.jsx";
import { Dots } from "../Dots.jsx";
import { ColleagueOrbit } from "./ColleagueOrbit.jsx";
import { CodeInput } from "./CodeInput.jsx";
import { CountryField } from "./CountryField.jsx";
import { Notice } from "./Notice.jsx";
import { QrCode } from "./QrCode.jsx";
import { countryNamed } from "./countries.js";
import { MIN_PASSWORD_LENGTH, passwordStrength } from "./password.js";
import { accountOf, refusalOf } from "./account.js";
import { LEGAL_LINKS as LINKS } from "../legal.js";
import { rememberRecoverySignIn } from "../../data/twoStep.js";
import styles from "./EntryForm.module.css";

// The statements a member accepts to join a firm: one for each document. An
// organisation's registration adds that its person may act for it.
const DOCUMENT_STATEMENTS = ["terms", "licence", "privacy", "personalData"];
const REGISTRATION_STATEMENTS = [...DOCUMENT_STATEMENTS, "authority"];
const emptyAgreements = () => ({ terms: false, licence: false, privacy: false, personalData: false, authority: false });
const AGREEMENT_OR_EARLIER = new Set(["email", "seat", "name", "country", "agree"]);
// Legal Machines mails a code to one organisation at most every 45 seconds.
const RESEND_SECONDS = 45;
// How often the wait for approval asks Legal Machines again.
const APPROVAL_EVERY_MS = 15000;

// Onboarding's entry (D-133): the organisation's email alone. Legal Machines
// knows the organisation: the seat, a login of the firm's with no person in it
// (D-204), then its password; the firm's administrator's seat then the code
// its authenticator app shows, or for an older one the code mailed to that
// email, while a colleague's seat is signed in and accepts the documents. It does not: the
// organisation registers, one thing a page (the owner, 2026-10-29, D-146) -
// its name, where it is registered, its agreement to the account Legal
// Machines makes for it; only then is Legal Machines asked - then confirms its
// email, sets its administrator's password, scans the authenticator's code on
// one page and enters the code it shows on the next, keeps its recovery codes
// and waits for Legal Machines' approval (D-030). A dot a step at the foot of
// the page. Signed in, onComplete takes onboarding on with the organisation's
// name.
const REGISTRATION = [
	["name", "Name"],
	["country", "Where it is registered"],
	["agree", "Agreement"],
	["confirm", "Email"],
	["newPassword", "Password"],
	["twoFactor", "Two-factor authentication"],
	["twoFactorCode", "Code from the app"],
	["recovery", "Recovery codes"],
	["yourSeat", "Your seat"],
];
// Signing in, one thing a page: the firm's email, the seat, its password and,
// for the administrator's seat, the code; a colleague's seat accepts the
// documents instead (D-204).
const SIGNING_IN = [
	["email", "Email"],
	["seat", "Seat"],
	["password", "Password"],
	["code", "Verification code"],
];
const COLLEAGUE_SIGNING_IN = [
	["email", "Email"],
	["seat", "Seat"],
	["password", "Password"],
	["memberAgree", "Agreement"],
];
// A colleague's seat with no authenticator yet sets one up after its
// agreements (D-215).
const COLLEAGUE_SETTING_UP = [
	...COLLEAGUE_SIGNING_IN,
	["twoFactor", "Two-step sign-in"],
	["twoFactorCode", "Code from the app"],
	["recovery", "Recovery codes"],
];
// What each step starts on: its first field, or else its title.
const FIRST_FIELD = {
	email: "email", seat: "seat", password: "password", code: "code", confirm: "code",
	name: "legalEntityName", newPassword: "password", twoFactorCode: "code", recover: "code",
};
// A recovery code as Legal Machines writes it: eight letters and digits,
// "7K4Q-M2XF".
const RECOVERY_LENGTH = 8;
const asRecoveryCode = (value) => {
	const plain = value.toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, RECOVERY_LENGTH);
	return plain.length > 4 ? `${plain.slice(0, 4)}-${plain.slice(4)}` : plain;
};
const LEVELS = { weak: "Weak", fair: "Fair", good: "Good", strong: "Strong" };
// The authenticator's key in groups of four, on two lines of equal length.
const keyLines = (secret) => {
	const groups = secret.match(/.{1,4}/g) ?? [];
	const half = Math.ceil(groups.length / 2);
	return [groups.slice(0, half).join(" "), groups.slice(half).join(" ")].filter(Boolean);
};

// Copies text where the window allows it: the clipboard, or else an input's
// own copy, which onboarding lets through (Onboarding.jsx stops the others).
async function copyText(text) {
	try {
		await navigator.clipboard.writeText(text);
		return true;
	} catch {
		const field = Object.assign(document.createElement("input"), { value: text, readOnly: true });
		Object.assign(field.style, { position: "fixed", opacity: "0", pointerEvents: "none" });
		document.body.append(field);
		field.select();
		const copied = document.execCommand("copy");
		field.remove();
		return copied;
	}
}

// Keep a step's title, question and action in one compact composition.
// Only overflowing question content scrolls; navigation remains at the foot.
function StepForm({ children, ...props }) {
	const parts = Children.toArray(children);
	return (
		<form className={`${styles.form} ${styles.pageForm}`} noValidate {...props}>
			{parts[0]}
			<div className={styles.contentStage}>
				<div className={styles.content}>{parts.slice(1, -1)}</div>
			</div>
			{parts.at(-1)}
		</form>
	);
}

export function EntryForm({ onComplete }) {
	const [account] = useState(() => accountOf(window.nativeApp));
	const [step, setStep] = useState("email");
	// "register" while a new organisation registers, "signin" while a seat
	// signs in, "colleague" once a colleague's seat did: their steps carry the
	// dots.
	const [journey, setJourney] = useState(null);
	const [email, setEmail] = useState("");
	// The seat's id as typed (D-204): drawn by Legal Machines, like papinian-x3k.
	const [seatLogin, setSeatLogin] = useState("");
	// The firm a colleague's seat signed in to, kept while they accept the
	// documents.
	const [signedInFirm, setSignedInFirm] = useState(null);
	const [password, setPassword] = useState("");
	// A new password is typed twice, and goes on only once both match (the
	// owner, 2026-10-29).
	const [repeat, setRepeat] = useState("");
	const [code, setCode] = useState("");
	// The second step of a sign-in: "authenticator" or "email"; and whether a
	// recovery code stands in for the authenticator.
	const [method, setMethod] = useState("email");
	const [byRecoveryCode, setByRecoveryCode] = useState(false);
	const [details, setDetails] = useState({ legalEntityName: "", country: "United States", registrationNumber: "" });
	// Each document is accepted by its own statement (the owner, 2026-10-29).
	const [agreed, setAgreed] = useState(emptyAgreements);
	// Two-factor setup: the key and link the authenticator app takes, and the
	// recovery codes it gave.
	const [twoFactor, setTwoFactor] = useState(null);
	const [twoFactorTry, setTwoFactorTry] = useState(0);
	// A forgotten password (D-215): the administrator's, reset by the code
	// mailed to the firm's email with the code their authenticator shows.
	const [twoStepCode, setTwoStepCode] = useState("");
	// A colleague's seat with no authenticator yet sets one up after its
	// agreements, with its own session (D-215), before it works.
	const [ownSetup, setOwnSetup] = useState(false);
	// The key was copied a moment ago: its label says so, then goes back.
	const [keyCopied, setKeyCopied] = useState(false);
	const [recoveryCodes, setRecoveryCodes] = useState([]);
	// The recovery codes were saved to a file or copied: only then can the
	// person say they kept them.
	const [kept, setKept] = useState(false);
	// What the organisation is once set up: "awaiting_approval" or "active".
	const [organisationState, setOrganisationState] = useState(null);
	// An organisation active at the end of its setup is signed in here then
	// (D-152): after its recovery codes, onboarding goes straight on, with no
	// code asked again.
	const [signedInHere, setSignedInHere] = useState(null);
	const [seat, setSeat] = useState(null);
	const [challengeId, setChallengeId] = useState(null);
	const [maskedEmail, setMaskedEmail] = useState(null);
	const [error, setError] = useState({ text: "", field: null, count: 0 });
	const [busy, setBusy] = useState(false);
	const [resendRemaining, setResendRemaining] = useState(0);
	// What just happened, said at the top of the entry (Notice.jsx): "done"
	// when it went as asked, "problem" when it did not. An empty text takes
	// it away.
	const [notice, setNoticeState] = useState({ text: "", tone: "done", count: 0 });
	const setNotice = (text, tone = "done") => setNoticeState((last) => ({ text, tone, count: last.count + 1 }));
	const fields = {
		email: useRef(null),
		seat: useRef(null),
		password: useRef(null),
		repeat: useRef(null),
		code: useRef(null),
		legalEntityName: useRef(null),
		country: useRef(null),
		registrationNumber: useRef(null),
		agreements: useRef(null),
		twoStep: useRef(null),
	};
	const headingRef = useRef(null);
	const groupRef = useRef(null);
	// An answer counts only while its request is the latest: going back or
	// leaving the page makes an answer still on its way stale.
	const attempt = useRef(0);
	const inFlight = useRef(false);
	const shownStep = useRef(step);
	// Which way the member goes, so a step arrives from that side.
	const forward = useRef(true);

	useEffect(() => () => {
		attempt.current += 1;
	}, []);
	// A new step arrives as one move from the side the member goes, as the
	// tour's pages do (forward from the right, back from the left; under
	// reduced motion it only fades), and starts on its first field.
	useLayoutEffect(() => {
		if (shownStep.current === step) return undefined;
		shownStep.current = step;
		const calm = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
		const from = calm ? { opacity: 0 } : { opacity: 0, transform: `translateX(${forward.current ? 28 : -28}px)` };
		const to = calm ? { opacity: 1 } : { opacity: 1, transform: "translateX(0)" };
		const motion = groupRef.current?.animate([from, to], { duration: calm ? 200 : 460, easing: "cubic-bezier(0.22, 1, 0.36, 1)" });
		const first = FIRST_FIELD[step];
		(first ? fields[first] : headingRef).current?.focus({ preventScroll: true });
		return () => motion?.cancel();
	}, [step]);
	useEffect(() => {
		if (!keyCopied) return undefined;
		const timer = setTimeout(() => setKeyCopied(false), 1600);
		return () => clearTimeout(timer);
	}, [keyCopied]);
	useEffect(() => {
		if (resendRemaining <= 0) return;
		const timer = setTimeout(() => setResendRemaining((value) => value - 1), 1000);
		return () => clearTimeout(timer);
	}, [resendRemaining]);
	// Two-factor setup asks Legal Machines for the authenticator's key as the
	// step opens.
	useEffect(() => {
		if ((step !== "twoFactor" && step !== "twoFactorCode") || twoFactor || (!ownSetup && !seat?.organisationId)) return undefined;
		let current = true;
		(ownSetup ? account.startOwnTwoFactor() : account.startTwoFactor(seat.organisationId)).then(
			(result) => current && setTwoFactor(result),
			(failure) => current && setError((last) => ({ text: refusalOf(failure), field: "twoFactor", count: last.count + 1 })),
		);
		return () => {
			current = false;
		};
	}, [step, twoFactor, twoFactorTry, seat?.organisationId, ownSetup]);
	// The wait for Legal Machines' approval (D-030) asks again every few seconds
	// while it shows, and when the window comes back. Once approved, sign-in
	// goes on by itself with the password typed for it, and the member only
	// enters the code that comes (2026-10-29: the wait never noticed approval).
	useEffect(() => {
		if (step !== "waiting" || !seat?.organisationId) return undefined;
		let current = true;
		let timer = null;
		let asking = false;
		const ask = async () => {
			if (!current || asking) return;
			clearTimeout(timer);
			if (document.visibilityState === "hidden") return;
			asking = true;
			try {
				const { approved } = await account.approved(seat.organisationId);
				if (!current) return;
				if (approved) {
					current = false;
					if (password) request(() => account.signIn(email, seat.seatId, password), "password", signedInAs);
					else {
						restart();
						setNotice("Your organisation is approved. Sign in to go on.");
					}
					return;
				}
			} catch {
				// The service did not say: it is asked again.
			} finally {
				asking = false;
			}
			if (current) timer = setTimeout(ask, APPROVAL_EVERY_MS);
		};
		timer = setTimeout(ask, APPROVAL_EVERY_MS);
		window.addEventListener("focus", ask);
		document.addEventListener("visibilitychange", ask);
		return () => {
			current = false;
			clearTimeout(timer);
			window.removeEventListener("focus", ask);
			document.removeEventListener("visibilitychange", ask);
		};
	}, [step, seat?.organisationId]);

	// What went wrong is said at the top until its field changes (Notice.jsx),
	// and the field is marked; nothing is written under it.
	const refuse = (text, field) => {
		setError((last) => ({ text, field, count: last.count + 1 }));
		fields[field]?.current?.focus();
		fields[field]?.current?.select?.();
	};
	const clear = () => setError((last) => (last.text || last.field ? { text: "", field: null, count: last.count } : last));
	const go = (next, back = false) => {
		if (back && AGREEMENT_OR_EARLIER.has(next)) setAgreed(emptyAgreements());
		forward.current = !back;
		setStep(next);
	};
	// Where each step's Back goes, with everything entered kept (the owner,
	// 2026-10-29). Before Legal Machines is asked, back through the details;
	// from the email's code back to the agreement, which asks again with what
	// changed; once the email is confirmed, back only within the setup after
	// it: the authenticator's pages to the password, its code to the QR code.
	const backOf = {
		seat: "email", password: "seat", code: "password",
		name: "email", country: "name", agree: "country", confirm: journey === "register" ? "agree" : "email",
		twoFactor: ownSetup ? "memberAgree" : "newPassword", twoFactorCode: "twoFactor",
		forgot: "password", recover: "forgot",
	};
	const back = () => {
		const previous = backOf[step];
		if (!previous) return;
		// An answer still on its way no longer moves the page.
		attempt.current += 1;
		inFlight.current = false;
		setBusy(false);
		clear();
		setNotice("");
		if (previous === "email") setJourney(null);
		go(previous, true);
	};
	// The steps Back reaches from here, one after another.
	const behind = [];
	for (let previous = backOf[step]; previous; previous = backOf[previous]) behind.push(previous);

	// One request to Legal Machines at a time; `then` moves the page on. A
	// refusal is said at `field`, or where `fieldOf` takes the page for it.
	const request = async (call, field, then, fieldOf) => {
		if (inFlight.current) return;
		inFlight.current = true;
		const mine = ++attempt.current;
		clear();
		setNotice("");
		setBusy(true);
		try {
			const result = await call();
			if (mine === attempt.current) await then(result);
		} catch (failure) {
			if (mine === attempt.current) {
				if (failure?.code === "account.setup_expired") {
					restart();
					setNotice(refusalOf(failure), "warning");
				} else refuse(refusalOf(failure), fieldOf?.(failure) ?? field);
			}
		} finally {
			if (mine === attempt.current) {
				inFlight.current = false;
				setBusy(false);
			}
		}
	};

	const done = (organisationName) => {
		setPassword("");
		setRepeat("");
		setCode("");
		setTwoFactor(null);
		setRecoveryCodes([]);
		// A registration just finished is celebrated on the welcome (the owner,
		// 2026-10-30).
		onComplete?.({ organisationName: organisationName || details.legalEntityName.trim() || null, registered: journey === "register", joined: journey === "colleague" });
	};
	const waitForCode = (next) => {
		setCode("");
		setResendRemaining(RESEND_SECONDS);
		go(next);
	};
	// A new organisation's next setting: its administrator's password, then
	// two-factor authentication.
	const goSetup = (setup) => {
		setJourney("register");
		setCode("");
		clear();
		if (setup === "two_factor") {
			setTwoFactor(null);
			go("twoFactor");
			return;
		}
		setPassword("");
		setRepeat("");
		go("newPassword");
	};

	// What a sign-in came to, by the password or after a confirmed email.
	const signedInAs = async (result) => {
		if (result?.organisationId) setSeat({ organisationId: result.organisationId, seatId: result.seatId });
		if (result?.maskedEmail) setMaskedEmail(result.maskedEmail);
		if (result?.status === "signed_in") {
			// Signed in by a recovery code (D-218): once Singularity opens, it
			// asks for the authenticator app again.
			if (result.usedRecoveryCode === true) rememberRecoverySignIn(result.recoveryCodesLeft);
			// A colleague's seat accepts the documents before it goes on (D-204).
			if (result.account?.role === "member" && journey !== "register") {
				setSignedInFirm(result.account?.organisationName ?? null);
				setJourney("colleague");
				setOwnSetup(result.twoFactorSetup === "required");
				return go("memberAgree");
			}
			return done(result.account?.organisationName);
		}
		if (result?.status === "two_factor_required") {
			setChallengeId(result.challengeId);
			setByRecoveryCode(false);
			if (result.method === "authenticator") {
				setMethod("authenticator");
				setCode("");
				return go("code");
			}
			setMethod("email");
			return waitForCode("code");
		}
		// Set up only in part: it goes on where it stopped.
		if (result?.status === "setup_required") return goSetup(result.setup);
		if (result?.status === "awaiting_approval") return go("waiting");
		// Registered, but its email is not confirmed yet: a new code to it.
		await account.resend(result.organisationId);
		waitForCode("confirm");
	};

	const everyStatement = (keys) => keys.every((key) => agreed[key]);
	const strength = useMemo(() => passwordStrength(password, [details.legalEntityName, email]), [password, details.legalEntityName, email]);
	// The repeat differs from the password once it is no longer the start of it.
	const mismatch = repeat !== "" && (repeat.length >= password.length ? repeat !== password : !password.startsWith(repeat));
	const newPasswordReady = strength.enough && repeat === password;
	// A step's button works once the step is filled in (the owner, 2026-10-29):
	// every field has something in it, a code all its digits, every statement
	// is accepted, the recovery codes kept. What is filled in wrong is said
	// when it is pressed; a new password says as it is typed what it still
	// needs.
	const ready = {
		email: email.trim() !== "",
		password: password !== "",
		code: byRecoveryCode ? code.replace(/-/g, "").length === RECOVERY_LENGTH : code.length === 6,
		confirm: code.length === 6,
		seat: seatLogin.trim() !== "",
		memberAgree: everyStatement(DOCUMENT_STATEMENTS),
		yourSeat: true,
		name: details.legalEntityName.trim() !== "",
		country: details.country.trim() !== "" && details.registrationNumber.trim() !== "",
		agree: everyStatement(REGISTRATION_STATEMENTS),
		newPassword: newPasswordReady,
		twoFactor: twoFactor !== null,
		twoFactorCode: twoFactor !== null && code.length === 6,
		recovery: kept,
		forgot: true,
		recover: code.length === 6 && newPasswordReady,
	}[step] === true;

	const submitEmail = (event) => {
		event.preventDefault();
		if (busy || !ready) return;
		const address = email.trim();
		if (!fields.email.current.validity.valid) {
			refuse("Enter a valid email address.", "email");
			return;
		}
		setEmail(address);
		// A known email signs in by a seat (D-204); a new one registers its
		// organisation.
		request(() => account.find(address), "email", ({ exists }) => {
			if (exists) {
				setJourney("signin");
				return go("seat");
			}
			setJourney("register");
			go("name");
		});
	};
	// The seat's id as Legal Machines draws it: small letters, a hyphen, no
	// spaces.
	const submitSeat = (event) => {
		event.preventDefault();
		if (busy || !ready) return;
		const typed = seatLogin.trim().toLowerCase().replace(/\s+/g, "");
		if (!/^[a-z0-9]+(-[a-z0-9]+)+$/.test(typed)) {
			refuse("Enter the seat as your firm’s administrator gave it, like papinian-x3k.", "seat");
			return;
		}
		setSeatLogin(typed);
		go("password");
	};
	// The administrator's forgotten password (D-215): a code to the firm's
	// email first; a colleague's is set again by the administrator.
	const submitForgot = (event) => {
		event.preventDefault();
		if (busy || !ready) return;
		request(() => account.requestRecovery(email), "forgot", () => {
			setCode("");
			setTwoStepCode("");
			setPassword("");
			setRepeat("");
			setResendRemaining(RESEND_SECONDS);
			go("recover");
		});
	};
	const submitRecover = (event) => {
		event.preventDefault();
		if (busy || !ready) return;
		request(() => account.resetPassword(email, code, twoStepCode.trim(), password), "code", () => {
			setPassword("");
			setRepeat("");
			setCode("");
			setTwoStepCode("");
			go("password");
			setNotice("Your password is changed. Sign in with it.");
		});
	};
	const submitPassword = (event) => {
		event.preventDefault();
		if (busy || !ready) return;
		request(() => account.signIn(email, seatLogin, password), "password", signedInAs);
	};
	const submitCode = (event) => {
		event.preventDefault();
		if (busy || !ready) return;
		if (step === "code") {
			request(() => account.verify(challengeId, code, seat?.organisationId, seat?.seatId, password, email), "code", signedInAs);
			return;
		}
		request(() => account.confirm(seat.organisationId, code), "code", async (result) => {
			if (result?.setup) return goSetup(result.setup);
			if (result?.state === "active") return signedInAs(await account.signIn(email, seat?.seatId ?? seatLogin, password));
			go("waiting");
		});
	};
	const resend = () => {
		if (resendRemaining || busy) return;
		setCode("");
		const then = () => {
			setResendRemaining(RESEND_SECONDS);
			setNotice("A new code is on its way");
		};
		if (step === "code") request(() => account.signIn(email, seatLogin || seat?.seatId, password), "code", (result) => {
			if (result?.challengeId) setChallengeId(result.challengeId);
			then();
		});
		else request(() => account.resend(seat.organisationId), "code", then);
	};
	const submitName = (event) => {
		event.preventDefault();
		if (busy || !ready) return;
		go("country");
	};
	const submitCountry = (event) => {
		event.preventDefault();
		if (busy || !ready) return;
		const country = countryNamed(details.country);
		if (!country) {
			refuse("Choose the country from the list.", "country");
			return;
		}
		setDetails((value) => ({ ...value, country: country.label }));
		go("agree");
	};
	// Legal Machines is asked only here, once the organisation has said who it
	// is and accepted the documents: it makes the account and mails a code.
	// Asked again after Back, it takes what changed and mails a new code.
	const submitAgreement = (event) => {
		event.preventDefault();
		if (busy || !ready) return;
		const registration = {
			legalEntityName: details.legalEntityName.trim(),
			country: details.country.trim(),
			registrationNumber: details.registrationNumber.trim(),
			email,
			agreements: {
				licenceAndTerms: agreed.licence && agreed.terms,
				privacyNotice: agreed.privacy,
				personalData: agreed.personalData,
				authority: agreed.authority,
				sharedMailbox: agreed.authority,
			},
		};
		request(() => account.create(registration), "agreements", (result) => {
			setSeat({ organisationId: result.organisationId, seatId: result.seatId });
			setMaskedEmail(result.maskedEmail ?? null);
			waitForCode("confirm");
		});
	};
	const submitNewPassword = (event) => {
		event.preventDefault();
		if (busy || !ready) return;
		request(() => account.setPassword(seat.organisationId, password), "password", (result) => goSetup(result?.setup ?? "two_factor"));
	};
	// The key scanned, the code the app shows is entered on a page of its own
	// (the owner, 2026-10-29).
	const submitScan = (event) => {
		event.preventDefault();
		if (busy || !ready) return;
		setCode("");
		clear();
		go("twoFactorCode");
	};
	const submitTwoFactor = (event) => {
		event.preventDefault();
		if (busy || !ready) return;
		if (ownSetup) {
			request(() => account.confirmOwnTwoFactor(code), "code", (result) => {
				setRecoveryCodes(result?.recoveryCodes ?? []);
				setKept(false);
				setCode("");
				go("recovery");
			});
			return;
		}
		request(() => account.confirmTwoFactor(seat.organisationId, code, seat.seatId, password, email), "code", (result) => {
			setRecoveryCodes(result?.recoveryCodes ?? []);
			setOrganisationState(result?.state ?? "awaiting_approval");
			setSignedInHere(result?.signedIn?.status === "signed_in" ? result.signedIn : null);
			setKept(false);
			setCode("");
			go("recovery");
		});
	};
	const submitRecovery = (event) => {
		event.preventDefault();
		if (busy || !ready) return;
		// A colleague's seat is signed in already: it goes on to the firm.
		if (ownSetup) {
			setOwnSetup(false);
			done(signedInFirm);
			return;
		}
		go("yourSeat");
	};
	// The administrator's own seat said once its setup is done (D-204): the
	// firm's email, this seat and its password sign in from then on.
	const submitYourSeat = (event) => {
		event.preventDefault();
		if (busy || !ready) return;
		if (organisationState === "active") {
			if (signedInHere) {
				signedInAs(signedInHere);
				return;
			}
			request(() => account.signIn(email, seat?.seatId ?? "", password), "yourSeat", signedInAs);
			return;
		}
		go("waiting");
	};
	const submitMemberAgree = (event) => {
		event.preventDefault();
		if (busy || !ready) return;
		if (ownSetup) {
			setTwoFactor(null);
			clear();
			go("twoFactor");
			return;
		}
		done(signedInFirm);
	};
	// Back to the email, which stays as typed.
	const restart = () => {
		attempt.current += 1;
		inFlight.current = false;
		setBusy(false);
		clear();
		setPassword("");
		setRepeat("");
		setCode("");
		setNotice("");
		setResendRemaining(0);
		setChallengeId(null);
		setSeat(null);
		setSeatLogin("");
		setSignedInFirm(null);
		setOwnSetup(false);
		setMaskedEmail(null);
		setJourney(null);
		setTwoFactor(null);
		setRecoveryCodes([]);
		setKept(false);
		setSignedInHere(null);
		go("email", true);
	};
	// The codes kept on paper (the owner, 2026-10-29), in a file or copied.
	const printRecoveryCodes = () => request(() => account.printRecoveryCodes(seat.organisationId), "recovery", (result) => {
		if (!result?.printed) return;
		setKept(true);
		setNotice("Codes opened for printing");
	});
	const saveRecoveryCodes = () => request(() => account.saveRecoveryCodes(seat.organisationId), "recovery", (result) => {
		if (!result?.saved) return;
		setKept(true);
		setNotice("Codes saved to a file");
	});
	const copyRecoveryCodes = async () => {
		clear();
		const copied = await copyText(recoveryCodes.join("\n"));
		if (copied) setKept(true);
		if (copied) setNotice("Codes copied");
		else setNotice("The codes couldn’t be copied", "problem");
	};

	const invalid = (field) => error.field === field;
	// A field that went wrong is described by the notice at the top.
	const describedBy = (field) => (invalid(field) ? "entry-notice" : undefined);
	const legal = (
		<div className={styles.legal}>
			<a href={LINKS.terms} target="_blank" rel="noreferrer">Terms of Use</a>
			<a href={LINKS.privacy} target="_blank" rel="noreferrer">Privacy Notice</a>
		</div>
	);
	const change = { label: "Change", onClick: restart };
	// After the first page the wordmark steps aside (the owner, 2026-10-29):
	// each page's own title is its heading, and the section is named by it.
	// Under it, the address the page is about, or a line of what to do, as a
	// phrase without a closing full stop.
	const heading = (title, { address, action, line } = {}) => (
		<div className={styles.codeHeading}>
			<h1 id="onboarding-title" className={styles.stepTitle} tabIndex={-1} ref={headingRef}>{title}</h1>
			{line ? <p className={styles.line}>{line}</p> : null}
			{address ? (
				<div className={styles.address}>
					<span title={address}>{address}</span>
					{action ? <button className={styles.textButton} type="button" onClick={action.onClick}>{action.label}</button> : null}
				</div>
			) : null}
		</div>
	);
	// A page's button says what the person confirms by pressing it (the owner,
	// 2026-10-29: "I've added the code"); only the first page's says Continue.
	const primary = (label, busyLabel = label) => (
		<button className={styles.primary} type="submit" disabled={busy || !ready} data-busy={busy || undefined}>
			<span className={styles.buttonLabel}>{busy ? busyLabel : label}</span>
		</button>
	);
	const input = (name, props) => (
		<input
			ref={fields[name]} id={`entry-${name}`} name={name} readOnly={busy}
			aria-invalid={invalid(name)} aria-describedby={describedBy(name)} {...props}
		/>
	);
	const detail = (key, label, props = {}) => (
		<div className={styles.field}>
			<label htmlFor={`entry-${key}`}>{label}</label>
			{input(key, {
				type: "text", value: details[key], maxLength: key === "legalEntityName" ? 200 : 120, ...props,
				onChange: (event) => { setDetails((value) => ({ ...value, [key]: event.target.value })); clear(); },
			})}
		</div>
	);
	const codeInput = () => (
		<CodeInput value={code} onChange={(value) => { setCode(value); clear(); }}
			inputRef={fields.code} invalid={invalid("code")} busy={busy} describedBy={describedBy("code")} />
	);

	const statement = (key, children) => (
		<div className={styles.statement}>
			<input
				type="checkbox" aria-labelledby={`entry-statement-${key}`} checked={agreed[key]} disabled={busy}
				onChange={(event) => { setAgreed((all) => ({ ...all, [key]: event.target.checked })); clear(); }}
			/>
			<span id={`entry-statement-${key}`}>{children}</span>
		</div>
	);
	const acceptance = (key, verb, href, name) => statement(key, <>{verb} the <a href={href} target="_blank" rel="noreferrer">{name}</a>.</>);
	const documentStatements = (
		<>
			{acceptance("terms", "I accept", LINKS.terms, "Terms of Use")}
			{acceptance("licence", "I accept", LINKS.licence, "Software License Agreement")}
			{acceptance("privacy", "I have read", LINKS.privacy, "Privacy Notice")}
			{statement("personalData", <>I consent to the <a href={LINKS.personalData} target="_blank" rel="noreferrer">processing of personal data</a>.</>)}
		</>
	);
	const to = maskedEmail ?? email;
	const name = details.legalEntityName.trim();
	// The address the seat signs in with, unseen beside a password: the Mac's
	// Passwords app, like any password manager, keeps and fills a password
	// under the account it belongs to.
	const accountName = (
		<input className={styles.hidden} type="email" name="username" autoComplete="username"
			value={email} readOnly tabIndex={-1} aria-hidden="true" />
	);
	// A new password and its repeat: how strong it is under the first, whether
	// they match under the second. The rules tell a password manager's
	// suggested password what Legal Machines takes.
	const passwordRules = `minlength: ${MIN_PASSWORD_LENGTH}; maxlength: 200;`;
	// Leaving a new password short of strong, the person is told at the top
	// what it still needs (the owner, 2026-10-29: recommend a strong one).
	const adviseOnPassword = () => {
		if (!password || !document.hasFocus() || strength.level === "good" || strength.level === "strong") return;
		setNotice(strength.level === "short" ? `The password needs ${strength.words}` : `${LEVELS[strength.level]} password: ${strength.words}`, "warning");
	};
	const newPasswordFields = (
		<div className={styles.fields}>
			{accountName}
			<div className={styles.field}>
				<label htmlFor="entry-password">Password</label>
				{input("password", {
					type: "password", autoComplete: "new-password", passwordrules: passwordRules, maxLength: 200, value: password,
					placeholder: `At least ${MIN_PASSWORD_LENGTH} characters`,
					onChange: (event) => { setPassword(event.target.value); clear(); },
					onBlur: adviseOnPassword,
				})}
			</div>
			<div className={styles.field}>
				<label htmlFor="entry-repeat">Repeat the password</label>
				{input("repeat", {
					type: "password", autoComplete: "new-password", passwordrules: passwordRules, maxLength: 200, value: repeat,
					"aria-invalid": mismatch || invalid("repeat"), "aria-describedby": mismatch || invalid("repeat") ? "entry-notice" : undefined,
					onChange: (event) => { setRepeat(event.target.value); clear(); },
				})}
			</div>
		</div>
	);

	// What the notice at the top says (Notice.jsx): what went wrong, while it
	// stands; else that the new passwords differ, while they do; else what
	// just happened, for a moment.
	const choosingPassword = step === "newPassword" || step === "recover";
	const message = error.text
		? { key: `error-${error.count}`, text: error.text, tone: "problem", stays: true }
		: choosingPassword && mismatch
			? { key: "mismatch", text: "The passwords don’t match", tone: "warning", stays: true }
			: notice.text
				? { key: `notice-${notice.count}`, text: notice.text, tone: notice.tone, stays: false }
				: null;

	let body;
	if (step === "waiting") {
		body = (
			<div className={styles.complete}>
				<h1 id="onboarding-title" className={styles.stepTitle} tabIndex={-1} ref={headingRef}>Waiting for approval</h1>
				<p className={styles.message}>Legal Machines is reviewing this organisation’s account. This page goes on by itself once it’s active, and we’ll write to {to}.</p>
				<button className={styles.primary} type="button" onClick={restart}><span className={styles.buttonLabel}>Back to sign in</span></button>
			</div>
		);
	} else if (step === "code" && method === "authenticator") {
		body = (
			<StepForm onSubmit={submitCode} aria-busy={busy} aria-label="Two-factor authentication">
				{heading("Two‑factor authentication", {
					line: byRecoveryCode ? "Enter one of your recovery codes" : "Enter the code your authenticator app shows",
				})}
				<div className={styles.field}>
					<label htmlFor="entry-code">{byRecoveryCode ? "Recovery code" : "Code"}</label>
					{byRecoveryCode
						? input("code", {
							type: "text", className: styles.code, autoComplete: "off", autoCapitalize: "characters", spellCheck: false,
							placeholder: "XXXX-XXXX", maxLength: 9, value: code,
							onChange: (event) => { setCode(asRecoveryCode(event.target.value)); clear(); },
						})
						: codeInput()}
				</div>
				<div className={styles.codeSupport}>
					<button className={styles.textButton} type="button" disabled={busy} onClick={() => { setByRecoveryCode(!byRecoveryCode); setCode(""); clear(); fields.code.current?.focus(); }}>
						{byRecoveryCode ? "Use the authenticator app" : "Use a recovery code"}
					</button>
				</div>
				{primary("Sign in", "Signing in…")}
			</StepForm>
		);
	} else if (step === "code" || step === "confirm") {
		body = (
			<StepForm onSubmit={submitCode} aria-busy={busy} aria-label={step === "code" ? "Sign in" : "Confirm your email"}>
				{heading(step === "code" ? "Check your email" : "Confirm your email", { address: `Code sent to ${to}`, action: change })}
				<div className={styles.field}>
					<label htmlFor="entry-code">Verification code</label>
					{codeInput()}
				</div>
				<div className={styles.codeSupport}>
					<button className={styles.textButton} type="button" onClick={resend} disabled={Boolean(resendRemaining) || busy}>
						{resendRemaining ? `Resend code in ${resendRemaining}s` : "Resend code"}
					</button>
				</div>
				{step === "code" ? primary("Sign in", "Signing in…") : primary("Confirm email", "Confirming…")}
			</StepForm>
		);
	} else if (step === "seat") {
		body = (
			<StepForm onSubmit={submitSeat} aria-busy={busy} aria-label="Your seat">
				{heading("Enter your seat", { address: email, action: change })}
				<div className={styles.field}>
					<label htmlFor="entry-seat">Seat</label>
					{input("seat", {
						type: "text", autoComplete: "off", autoCapitalize: "none", autoCorrect: "off", spellCheck: false,
						placeholder: "papinian-x3k", maxLength: 64, value: seatLogin,
						onChange: (event) => { setSeatLogin(event.target.value); clear(); },
					})}
				</div>
				<p className={styles.line}>Your firm’s administrator gives you your seat with its password</p>
				{primary("Continue")}
			</StepForm>
		);
	} else if (step === "password") {
		body = (
			<StepForm onSubmit={submitPassword} aria-busy={busy} aria-label="Sign in">
				{heading("Enter your password", { address: seatLogin })}
				{accountName}
				<div className={styles.field}>
					<label htmlFor="entry-password">Password</label>
					{input("password", {
						type: "password", autoComplete: "current-password", maxLength: 200, value: password,
						onChange: (event) => { setPassword(event.target.value); clear(); },
					})}
				</div>
				<div className={styles.codeSupport}>
					<button className={styles.textButton} type="button" disabled={busy} onClick={() => { clear(); setNotice(""); go("forgot"); }}>Forgot your password?</button>
				</div>
				{primary("Sign in", "Signing in…")}
			</StepForm>
		);
	} else if (step === "forgot") {
		body = (
			<StepForm onSubmit={submitForgot} aria-busy={busy} aria-label="Forgot your password">
				{heading("Forgot your password?", { address: email })}
				<p className={styles.line}>A colleague’s password is set again by your firm’s administrator, on your page in Team.</p>
				<p className={styles.line}>The administrator gets a code at the firm’s email, and enters it with the code from their authenticator app.</p>
				{primary("Send the code", "Sending…")}
			</StepForm>
		);
	} else if (step === "recover") {
		body = (
			<StepForm onSubmit={submitRecover} aria-busy={busy} aria-label="A new password">
				{heading("Choose a new password", { line: `The code went to ${to}` })}
				<div className={styles.field}>
					<label htmlFor="entry-code">Code from the email</label>
					{codeInput()}
				</div>
				<div className={styles.field}>
					<label htmlFor="entry-twoStep">Code from your authenticator app, or a recovery code</label>
					{input("twoStep", {
						type: "text", inputMode: "text", autoComplete: "one-time-code", autoCapitalize: "characters", spellCheck: false,
						maxLength: 12, value: twoStepCode, placeholder: "123456",
						onChange: (event) => { setTwoStepCode(event.target.value); clear(); },
					})}
				</div>
				{newPasswordFields}
				{primary("Set the new password", "Setting…")}
			</StepForm>
		);
	} else if (step === "name") {
		body = (
			<StepForm onSubmit={submitName} aria-label="Your organisation’s name">
				{heading("What’s your organisation called?", { address: email })}
				{detail("legalEntityName", "Legal name", { autoComplete: "organization", placeholder: "As it is registered" })}
				{primary("That’s our name")}
			</StepForm>
		);
	} else if (step === "country") {
		body = (
			<StepForm onSubmit={submitCountry} aria-label="Where it is registered">
				{heading("Where is it registered?", { line: name })}
				<div className={styles.fields}>
					<CountryField
						className={styles.field} id="entry-country" label="Country" value={details.country}
						inputRef={fields.country} invalid={invalid("country")} busy={busy} describedBy={describedBy("country")}
						onChange={(text) => { setDetails((value) => ({ ...value, country: text })); clear(); }}
					/>
					{detail("registrationNumber", "Registration number", { autoComplete: "off", spellCheck: false })}
				</div>
				{primary("That’s where we’re registered")}
			</StepForm>
		);
	} else if (step === "agree") {
		body = (
			<StepForm onSubmit={submitAgreement} aria-busy={busy} aria-label="Create the account">
				{heading("Create your organisation’s account", { line: name })}
				{/* The organisation accepts Singularity's documents here, each by its own statement (D-145). */}
				<fieldset className={`${styles.statements} ${styles.first}`} ref={fields.agreements} tabIndex={-1} aria-invalid={invalid("agreements")}>
					<legend className={styles.hidden}>Agreements</legend>
					{documentStatements}
					{statement("authority", "I may act for this organisation, and its email is a shared mailbox.")}
				</fieldset>
				{primary("Create the account", "Creating…")}
			</StepForm>
		);
	} else if (step === "newPassword") {
		body = (
			<StepForm onSubmit={submitNewPassword} aria-busy={busy} aria-label="Create a password">
				{heading("Create a password", { line: `For the administrator of ${name || "your organisation"}` })}
				{newPasswordFields}
				{primary("Set the password", "Setting…")}
			</StepForm>
		);
	} else if (step === "twoFactor") {
		body = (
			<StepForm onSubmit={submitScan} aria-busy={twoFactor === null} aria-label="Two-factor authentication">
				{heading("Turn on two‑factor authentication", { line: "Scan this with an authenticator app" })}
				<div className={styles.qr}>
					{twoFactor ? <QrCode text={twoFactor.uri} label="QR code for your authenticator app" /> : null}
					{twoFactor === null && invalid("twoFactor") ? (
						<button className={styles.textButton} type="button" onClick={() => { clear(); setTwoFactorTry((value) => value + 1); }}>Try again</button>
					) : null}
				</div>
				{/* The key copies itself when clicked (the owner, 2026-10-29). */}
				{twoFactor ? (
					<div className={styles.key}>
						<span id="entry-key-label" aria-live="polite">{keyCopied ? "Key copied" : "Or enter this key"}</span>
						<button
							className={styles.keyValue} type="button" title="Copy the key" aria-describedby="entry-key-label"
							onClick={async () => { clear(); setKeyCopied(await copyText(twoFactor.secret)); }}
						>
							{keyLines(twoFactor.secret).map((line, at) => <span key={at}>{line}</span>)}
						</button>
					</div>
				) : null}
				{primary("I’ve added the code")}
			</StepForm>
		);
	} else if (step === "twoFactorCode") {
		body = (
			<StepForm onSubmit={submitTwoFactor} aria-busy={busy} aria-label="Code from the app">
				{heading("Enter the code from the app", { line: "Your authenticator app shows six digits, new every 30 seconds" })}
				<div className={styles.field}>
					<label htmlFor="entry-code">Code</label>
					{codeInput()}
				</div>
				{primary("Turn on two‑factor", "Checking…")}
			</StepForm>
		);
	} else if (step === "recovery") {
		body = (
			<StepForm onSubmit={submitRecovery} aria-busy={busy} aria-label="Recovery codes">
				{heading("Save your recovery codes", { line: "If you lose your authenticator app, each code signs you in once" })}
				<ol className={styles.recoveryCodes} aria-label="Recovery codes">
					{recoveryCodes.map((item) => <li key={item}>{item}</li>)}
				</ol>
				<div className={styles.keep}>
					<button className={styles.textButton} type="button" disabled={busy} onClick={printRecoveryCodes}>Print</button>
					<button className={styles.textButton} type="button" disabled={busy} onClick={saveRecoveryCodes}>Save as file</button>
					<button className={styles.textButton} type="button" disabled={busy} onClick={copyRecoveryCodes}>Copy</button>
				</div>
				{primary("I’ve saved the codes")}
			</StepForm>
		);
	} else if (step === "yourSeat") {
		body = (
			<StepForm onSubmit={submitYourSeat} aria-busy={busy} aria-label="Your seat">
				{heading("Your seat", { line: "Sign in with your firm’s email, this seat and your password" })}
				<div className={styles.key}>
					<span id="entry-seat-label" aria-live="polite">{keyCopied ? "Seat copied" : "Legal Machines made it for you"}</span>
					<button
						className={styles.keyValue} type="button" title="Copy the seat" aria-describedby="entry-seat-label"
						onClick={async () => { clear(); setKeyCopied(await copyText(seat?.seatId ?? "")); }}
					>
						<span>{seat?.seatId}</span>
					</button>
				</div>
				{primary("Continue", "Signing in…")}
			</StepForm>
		);
	} else if (step === "memberAgree") {
		body = (
			<StepForm onSubmit={submitMemberAgree} aria-busy={busy} aria-label="Join the firm">
				{heading(signedInFirm ? `Welcome to ${signedInFirm}` : "Welcome to your firm", { line: "Please confirm" })}
				<fieldset className={`${styles.statements} ${styles.first}`} ref={fields.agreements} tabIndex={-1} aria-invalid={invalid("agreements")}>
					<legend className={styles.hidden}>Agreements</legend>
					{documentStatements}
				</fieldset>
				{primary("Continue")}
			</StepForm>
		);
	} else {
		body = (
			<form className={styles.form} noValidate onSubmit={submitEmail} aria-busy={busy} aria-label="Sign in">
				<div className={styles.field}>
					<label className={styles.hidden} htmlFor="entry-email">Work email</label>
					{input("email", {
						type: "email", autoComplete: "username", autoCapitalize: "none", spellCheck: false,
						placeholder: "legal@yourfirm.com", required: true, maxLength: 254, value: email,
						onChange: (event) => { setEmail(event.target.value); clear(); },
					})}
				</div>
				{primary("Continue", "Continuing…")}
				{legal}
			</form>
		);
	}

	// The cover keeps its wordmark and legal links. Every later page centres
	// its title, question and action together, with Back and progress at the
	// window's foot.
	const steps = journey === "register" ? REGISTRATION : journey === "colleague" ? (ownSetup ? COLLEAGUE_SETTING_UP : COLLEAGUE_SIGNING_IN)
		: journey === "signin" ? SIGNING_IN : [];
	const at = steps.findIndex(([name]) => name === step || journey === "signin" && step === "confirm" && name === "code");
	const cover = step === "email";
	return (
		<div className={styles.entry}>
			<Notice message={message} />
			{backOf[step] ? <BackButton onClick={back} disabled={busy && step === "twoFactorCode"} /> : null}
			<div className={styles.group} data-entry-step={step} data-layout={cover ? "cover" : "page"} ref={groupRef}>
				{cover && (
					<header className={styles.intro} data-entry-step={step}>
						<div className={styles.titleStage}>
							<ColleagueOrbit />
							<h1 id="onboarding-title" className={styles.title}>Singularity</h1>
						</div>
						<p className={styles.description}>A shared workspace for legal work, where people and AI are equal colleagues.</p>
					</header>
				)}
				{body}
			</div>
			{at >= 0 ? (
				<Dots
					steps={steps.map(([, title]) => title)} current={at} label={journey === "register" ? "Registration" : "Signing in"}
					canGo={(index) => !(busy && step === "twoFactorCode") && behind.includes(steps[index][0])}
					onGo={(index) => {
						attempt.current += 1;
						inFlight.current = false;
						setBusy(false);
						setNotice("");
						clear();
						if (steps[index][0] === "email") setJourney(null);
						go(steps[index][0], true);
					}}
				/>
			) : cover ? null : (
				<div className={styles.foot}>{legal}</div>
			)}
		</div>
	);
}
