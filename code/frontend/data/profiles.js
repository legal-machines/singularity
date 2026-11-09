// Everything here is fictional: the addresses are on the reserved `.example`
// domain and the telephone numbers in the 555-01xx range kept for fiction. A
// person's years agree with the fixture's age and years of experience
// (public/mock/team/profiles.json). An AI colleague's facts are the ones its
// work depends on: where it may work and read, what it follows and can do, what
// it runs on and how its requests are protected, to whom it answers, how far its
// probation has come, and what it costs.

export const firmDomain = "marlowefinch.example";

export const personFacts = {
	m_gabriel: {
		email: "gabriel.moore@marlowefinch.example",
		phone: "+1 (212) 555-0142",
		office: "New York",
		born: "June 9, 1974",
		joined: "2009",
		education: ["J.D., Columbia Law School, 1999", "B.A., Economics, Georgetown University, 1996"],
		admissions: ["New York, 2000", "Delaware, 2004"],
		languages: ["English", "French"],
	},
	m_priya: {
		email: "priya.raman@marlowefinch.example",
		phone: "+1 (212) 555-0118",
		office: "New York",
		born: "February 2, 1969",
		joined: "2003",
		education: ["J.D., Yale Law School, 1994", "B.A., History, Brown University, 1991"],
		admissions: ["New York, 1995", "U.S. Court of Appeals for the Second Circuit, 1998"],
		languages: ["English", "Hindi", "Tamil"],
	},
	m_margaret: {
		email: "margaret.chen@marlowefinch.example",
		phone: "+1 (415) 555-0163",
		office: "San Francisco",
		born: "November 18, 1955",
		joined: "2012",
		education: ["J.D., Stanford Law School, 1981", "B.A., English, University of California, Berkeley, 1978"],
		admissions: ["California, 1981", "U.S. Court of Appeals for the Ninth Circuit, 1984"],
		languages: ["English", "Mandarin"],
	},
	m_thomas: {
		email: "thomas.bennett@marlowefinch.example",
		phone: "+1 (212) 555-0127",
		office: "New York",
		born: "April 3, 1960",
		joined: "1998",
		education: ["LL.M. in Taxation, New York University School of Law, 1986", "J.D., New York University School of Law, 1985", "B.S., Accounting, Cornell University, 1982"],
		admissions: ["New York, 1986"],
		languages: ["English"],
	},
	m_raymond: {
		email: "raymond.brooks@marlowefinch.example",
		phone: "+1 (202) 555-0171",
		office: "Washington, D.C.",
		born: "August 21, 1952",
		joined: "2015",
		education: ["J.D., Howard University School of Law, 1977", "B.A., Political Science, Morehouse College, 1974"],
		admissions: ["District of Columbia, 1978", "Maryland, 1980"],
		languages: ["English"],
	},
	m_omar: {
		email: "omar.haddad@marlowefinch.example",
		phone: "+1 (202) 555-0135",
		office: "Washington, D.C.",
		born: "January 27, 1965",
		joined: "2011",
		education: ["J.D., Georgetown University Law Center, 1991", "B.A., Government, University of Virginia, 1988"],
		admissions: ["Virginia, 1991", "District of Columbia, 1993"],
		languages: ["English", "Arabic"],
	},
	m_amina: {
		email: "amina.okafor@marlowefinch.example",
		phone: "+1 (212) 555-0154",
		office: "New York",
		born: "May 12, 1981",
		joined: "2016",
		education: ["J.D., University of Pennsylvania Carey Law School, 2007", "B.S., Computer Science, Rutgers University, 2004"],
		admissions: ["New York, 2008", "California, 2015"],
		certifications: ["CIPP/US, International Association of Privacy Professionals"],
		languages: ["English", "Yoruba", "French"],
	},
	m_evelyn: {
		email: "evelyn.hart@marlowefinch.example",
		phone: "+1 (202) 555-0109",
		office: "Washington, D.C.",
		born: "September 30, 1987",
		joined: "2019",
		education: ["J.D., George Washington University Law School, 2014", "B.A., Sociology, University of Michigan, 2011"],
		admissions: ["Maryland, 2014", "District of Columbia, 2015"],
		languages: ["English", "Spanish"],
	},
	m_noah: {
		email: "noah.kim@marlowefinch.example",
		phone: "+1 (212) 555-0186",
		office: "New York",
		born: "July 7, 1995",
		joined: "2021",
		education: ["J.D., Fordham University School of Law, 2020", "B.S., Mechanical Engineering, Carnegie Mellon University, 2016"],
		admissions: ["New York, 2021"],
		languages: ["English", "Korean"],
	},
	m_lena: {
		email: "lena.ortiz@marlowefinch.example",
		phone: "+1 (212) 555-0191",
		office: "New York",
		born: "March 14, 2000",
		joined: "2022",
		education: ["Paralegal Certificate, NYU School of Professional Studies, 2022", "B.A., Political Science, Hunter College, 2022"],
		certifications: ["Certified Paralegal, NALA, 2024"],
		languages: ["English", "Spanish"],
	},
};

// An AI colleague's working facts. `answersTo` is the member it answers to;
// `reads` is what it may open; `follows` the firm's instructions it works
// from; `can` what it does in its area; `runs` and `protection` where and how
// its model runs; `attention` the lawyer attention its results cost; `spend`
// its usage this month, as Usage counts it.
export const colleagueFacts = {
	m_juno: {
		email: "juno@marlowefinch.example",
		joined: "October 2025",
		probation: "Completed in January 2026",
		answersTo: "m_evelyn",
		reads: "The Documents of the Matters it works on, and the mail it is copied on in them",
		follows: ["NDA Playbook 2026", "The firm's charter for commercial contracts"],
		can: ["Reads an incoming NDA against the Playbook", "Marks it up and explains every change", "Drafts the cover note to the counterparty", "Files the result to its Matter"],
		runs: "Always on, on the firm's server",
		protection: "Client names and figures are masked before a request leaves the firm (Papinian Guard)",
		attention: "1 lawyer action per accepted result, at the median",
		spend: "$184.20 this month across 2 Matters",
	},
	m_teodor: {
		email: "teodor@marlowefinch.example",
		joined: "February 2026",
		probation: "Under way: its memos are reviewed in full until June 2026",
		answersTo: "m_evelyn",
		reads: "The Documents of the Matters it works on, and public export regulations",
		follows: ["Export-control memo template", "Export classification checklist"],
		can: ["Classifies an item under the Export Administration Regulations", "Researches licence requirements by destination", "Writes a research memo with its sources", "Flags what needs a licence application"],
		runs: "Always on, on the firm's server",
		protection: "Client names and figures are masked before a request leaves the firm (Papinian Guard)",
		attention: "3 lawyer actions per accepted result, at the median",
		spend: "$96.40 this month across 1 Matter",
	},
	m_iris: {
		email: "iris@marlowefinch.example",
		joined: "June 2025",
		probation: "Completed in September 2025",
		answersTo: "m_priya",
		reads: "The Documents and court filings of the Matters it keeps the docket for",
		follows: ["The firm's docketing rules", "Local rules of the courts on its Matters"],
		can: ["Computes deadlines from court rules and orders", "Sets reminders for the team and local counsel", "Watches the docket for new filings", "Records every deadline with its source"],
		runs: "On a local model, on the firm's server",
		protection: "Its requests never leave the firm",
		attention: "0 lawyer actions per kept deadline, at the median",
		spend: "$12.10 this month in local processing",
	},
	m_themis: {
		email: "themis@marlowefinch.example",
		joined: "March 2025",
		probation: "Completed in July 2025",
		answersTo: "m_gabriel",
		reads: "Every commercial Matter of the practice, on escalation",
		follows: ["The firm's commercial playbooks", "The firm's charter for commercial contracts"],
		can: ["Decides the firm's position on a commercial clause", "Reviews every point past a red line", "Keeps the playbooks level with the market", "Returns work with its reasons"],
		runs: "Always on, on the firm's server",
		protection: "Client names and figures are masked before a request leaves the firm (Papinian Guard)",
		attention: "1 lawyer action per accepted escalation, at the median",
		spend: "$412.75 this month across 6 Matters",
	},
};
