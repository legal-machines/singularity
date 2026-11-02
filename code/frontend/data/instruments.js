// MOCK DATA - the text of the Sablewood NDA as Unicorn marked it up (version 2),
// fictional throughout (AG-12). A paragraph is a list of runs: plain text, a
// deletion ({ del }) or an insertion ({ ins }), each change with the note
// that gives its reason and the clause of the Playbook it rests on. Section
// 2.1 cites Exhibit B, which is not in the Matter's Documents.

export const instruments = {
	doc_sablewood_markup: {
		title: "Mutual Non-Disclosure Agreement",
		parties: "Halvane Robotics, Inc. and Sablewood Distribution LLC",
		file: "Sablewood NDA - Halvane comments v2.docx",
		workProductId: "wp_sablewood_v2",
		sections: [
			{
				number: "1",
				heading: "Definitions",
				clauses: [
					{
						number: "1.3",
						runs: [
							"“Confidential Information” means ",
							{ del: "all information disclosed by either party, in any form", ins: "all non-public information disclosed by either party that is marked or identified as confidential at the time of disclosure, or that a reasonable person would understand to be confidential", note: "n1" },
							", including technical data, product designs, pricing and customer lists.",
						],
					},
				],
			},
			{
				number: "2",
				heading: "Purpose",
				clauses: [
					{
						number: "2.1",
						runs: [
							"The Recipient shall use Confidential Information solely to evaluate a possible acquisition of Sablewood Distribution LLC by Halvane Robotics, Inc. on the terms outlined in ",
							{ cite: "Exhibit B", note: "n2" },
							" (the “Purpose”) and for no other purpose.",
						],
					},
				],
			},
			{
				number: "3",
				heading: "Obligations",
				clauses: [
					{
						number: "3.1",
						runs: ["The Recipient shall protect the Discloser's Confidential Information with at least the degree of care it uses to protect its own confidential information of a similar nature, and in no event less than reasonable care."],
					},
					{
						number: "3.2",
						runs: ["The Recipient may disclose Confidential Information only to its employees, officers and professional advisers who need to know it for the Purpose and who are bound by obligations of confidentiality no less protective than those in this Agreement."],
					},
				],
			},
			{
				number: "4",
				heading: "Term",
				clauses: [
					{
						number: "4.2",
						runs: [
							"This Agreement shall remain in effect for ",
							{ del: "five (5)", ins: "three (3)", note: "n3" },
							" years from the Effective Date. The obligations in Section 3 survive its expiry for two (2) years.",
						],
					},
				],
			},
			{
				number: "6",
				heading: "Residuals",
				clauses: [
					{
						number: "6.3",
						runs: [
							{ del: "Recipient may use Residual Information for any purpose. “Residual Information” means information retained in the unaided memory of the Recipient's personnel.", note: "n4" },
							{ ins: "[Intentionally omitted.]", note: "n4" },
						],
					},
				],
			},
			{
				number: "8",
				heading: "Non-solicitation",
				clauses: [
					{
						number: "8.1",
						runs: [
							"For twelve (12) months from the Effective Date, Recipient shall not ",
							{ del: "solicit or hire any employee of Discloser", ins: "actively solicit for employment any employee of Discloser with whom it had contact in connection with the Purpose", note: "n5" },
							"; general solicitations not targeted at the Discloser's employees do not breach this Section.",
						],
					},
				],
			},
			{
				number: "11",
				heading: "General",
				clauses: [
					{
						number: "11.4",
						runs: [
							"This Agreement shall be governed by the laws of the State of New York",
							{ mark: "", note: "n6" },
							", without regard to its conflict of laws rules.",
						],
					},
				],
			},
		],
		notes: {
			n1: { authorId: "m_juno", kind: "change", text: "Limited to what is marked or plainly confidential.", source: "Playbook 2.1: preferred position." },
			n2: { authorId: "m_juno", kind: "unchecked", text: "Exhibit B is cited here but is not in the Matter's Documents, so the Purpose could not be checked.", source: "Lena is asking Sablewood's bankers for it." },
			n3: { authorId: "m_juno", kind: "change", text: "Three years is the longest term the Playbook accepts.", source: "Playbook 4.1: preferred two, acceptable up to three." },
			n4: { authorId: "m_juno", kind: "change", text: "Struck at Evelyn's word: the gripper designs are what it would let Sablewood keep.", source: "Playbook 6.1, hardware clients. Kitsune confirmed: no escalation." },
			n5: { authorId: "m_juno", kind: "change", text: "Narrowed to active solicitation of Sablewood's employees Halvane deals with in diligence; general job ads don't count.", source: "Playbook 8.2. Evelyn, 9:52 AM." },
			n6: { authorId: "m_juno", kind: "kept", text: "New York law kept, as Evelyn asked.", source: "Evelyn, 9:52 AM." },
		},
	},
};
