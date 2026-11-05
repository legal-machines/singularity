// Singularity's legal documents (D-144, D-145): the site serves the version
// last published in the Internal Lab, Admin > Legal, at these addresses.
// The native bridge opens a link with a target in the default browser.
const LEGAL = "https://legalmachines.org/singularity/legal";

export const LEGAL_LINKS = {
	licence: `${LEGAL}/software-licence`,
	// Its section 4: the protection model, its own license, and that masking is
	// not guaranteed and Legal Machines is not liable for what it misses.
	licenceModels: `${LEGAL}/software-licence#section-4`,
	terms: `${LEGAL}/terms`,
	privacy: `${LEGAL}/privacy`,
	personalData: `${LEGAL}/personal-data-consent`,
	// A design partner's seat accepts it before its first bug report or work
	// log (D-257).
	diagnostics: `${LEGAL}/diagnostics-consent`,
};

// Each document's title as the site publishes it. The protection model comes
// from Legal Machines under the Software License Agreement's section 4, which
// the firm accepts before anything downloads (D-195).
export const LEGAL_TITLES = {
	licence: "Software License Agreement",
	licenceModels: "Software License Agreement, section 4",
	terms: "Terms of Use",
	privacy: "Privacy Notice",
	personalData: "Personal Data Consent",
	diagnostics: "Diagnostics Consent",
};

// The published results of client data protection's latest test (D-195):
// what the protection slide's figure rests on, open to anyone.
export const PROTECTION_TEST = "https://legalmachines.org/disclosure/protection-test";
