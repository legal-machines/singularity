// The practices of law, by the names an American firm gives them, and the
// ways lawyers type them: abbreviations, synonyms, British and American
// words, the singular and the plural. A member who types "MA" means "Mergers
// & Acquisitions", and the field says so once they leave it. What is not
// recognised is left exactly as typed.
//
// Each practice also names the work onboarding's demonstration plays for it
// (emulation/scenarios.js): its own, or the one it shares with a practice
// that does the same work, so a member sees the work of their own field.
export const PRACTICES = [
	// Corporate and transactions.
	{ name: "Mergers & Acquisitions", family: "ma", aliases: ["M&A", "MnA", "mergers", "acquisitions", "merger and acquisition", "mergers acquisitions", "M&A transactions", "public M&A", "private M&A", "cross-border M&A", "corporate M&A", "public company M&A", "takeovers", "takeover", "public takeovers", "divestitures", "carve-outs", "dispositions", "buy-side", "sell-side"] },
	{ name: "Private Equity", family: "privateEquity", aliases: ["PE", "buyouts", "buyout", "LBO", "LBOs", "leveraged buyouts", "financial sponsors", "sponsors", "PE transactions", "growth equity", "secondaries"] },
	{ name: "Venture Capital & Emerging Companies", family: "venture", aliases: ["VC", "venture capital", "venture", "venture financing", "emerging companies", "emerging growth companies", "startups", "start-ups", "start ups", "startup", "early-stage companies", "seed financing", "ECVC", "emerging companies and venture capital"] },
	{ name: "Joint Ventures & Strategic Alliances", family: "jointVenture", aliases: ["JV", "JVs", "joint ventures", "joint venture", "strategic alliances", "alliances", "collaborations"] },
	{ name: "Corporate", family: "ma", aliases: ["corporate law", "general corporate", "corporate and commercial", "corporate transactions", "company law", "business law", "business", "transactional", "transactions"] },
	{ name: "Corporate Governance", family: "governance", aliases: ["governance", "board advisory", "boards", "directors duties", "fiduciary duties"] },
	{ name: "Shareholder Activism", family: "governance", aliases: ["activism", "activist defense", "proxy contests", "proxy fights"] },
	{ name: "Franchising & Distribution", family: "distribution", aliases: ["franchising", "franchise", "franchises", "distribution", "distribution agreements", "dealerships", "agency and distribution"] },
	{ name: "Commercial Contracts", family: "commercial", aliases: ["commercial", "commercial law", "contracts", "contract law", "commercial agreements", "commercial transactions", "supply agreements", "supply chain", "sourcing"] },

	// Capital markets and finance.
	{ name: "Capital Markets", family: "capitalMarkets", aliases: ["ECM", "DCM", "equity capital markets", "debt capital markets", "securities", "securities offerings", "securities law", "securities regulation", "IPO", "IPOs", "initial public offerings", "public offerings", "listings", "high yield", "bonds", "SPAC", "SPACs", "de-SPAC", "SEC reporting", "public company advisory", "public companies", "liability management", "convertible notes", "convertibles"] },
	{ name: "Public Finance", family: "publicFinance", aliases: ["municipal finance", "municipal bonds", "munis", "bond counsel", "tax-exempt bonds"] },
	{ name: "Banking & Finance", family: "finance", aliases: ["finance", "banking", "B&F", "finance law", "banking law", "lending", "loans", "commercial lending", "corporate lending", "syndicated lending", "syndicated loans", "credit facilities", "bank finance", "debt finance", "debt financing", "secured lending", "secured transactions", "UCC", "asset-based lending", "ABL", "private credit", "direct lending", "trade finance"] },
	{ name: "Leveraged Finance", family: "leveragedFinance", aliases: ["lev fin", "levfin", "acquisition finance", "acquisition financing", "LBO financing", "sponsor finance", "leveraged lending"] },
	{ name: "Project Finance", family: "projectFinance", aliases: ["PF", "project financing", "infrastructure finance", "projects finance"] },
	{ name: "Structured Finance & Securitization", family: "securitization", aliases: ["structured finance", "securitization", "securitisation", "securitizations", "ABS", "asset-backed securities", "CLO", "CLOs", "CDO", "MBS", "CMBS", "RMBS", "structured products", "warehouse lending"] },
	{ name: "Derivatives", family: "derivatives", aliases: ["swaps", "ISDA", "OTC derivatives", "hedging", "commodities", "commodity derivatives", "CFTC"] },
	{ name: "Asset Finance", family: "assetFinance", aliases: ["equipment finance", "equipment leasing", "aircraft finance", "aviation finance", "ship finance", "shipping finance", "rail finance", "transportation finance"] },
	{ name: "Fund Finance", family: "fundFinance", aliases: ["subscription lines", "subscription facilities", "NAV facilities", "NAV lending"] },
	{ name: "Islamic Finance", family: "islamicFinance", aliases: ["sharia finance", "shariah finance", "sukuk"] },
	{ name: "Bankruptcy & Restructuring", family: "restructuring", aliases: ["bankruptcy", "restructuring", "insolvency", "restructuring and insolvency", "insolvency and restructuring", "R&I", "financial restructuring", "Chapter 11", "Ch 11", "chapter eleven", "distressed", "distressed debt", "distressed M&A", "creditors rights", "workouts", "workout", "turnaround", "business restructuring", "special situations", "bankruptcy litigation"] },
	{ name: "Consumer Bankruptcy", family: "consumerBankruptcy", aliases: ["personal bankruptcy", "Chapter 7", "Chapter 13", "debt relief", "debt settlement"] },
	{ name: "Investment Funds", family: "funds", aliases: ["funds", "fund formation", "private funds", "private investment funds", "investment management", "asset management", "hedge funds", "hedge fund", "registered funds", "mutual funds", "40 Act", "Investment Company Act", "alternative investments", "fund regulatory", "investment advisers", "investment advisors", "RIA", "funds and investment management", "private fund formation"] },
	{ name: "Financial Services Regulatory", family: "finReg", aliases: ["financial regulation", "financial regulatory", "finreg", "financial services", "financial services regulation", "banking regulation", "bank regulatory", "bank regulation", "broker-dealer", "broker-dealers", "consumer finance", "consumer financial services", "CFPB", "AML", "anti-money laundering", "BSA"] },
	{ name: "Fintech & Payments", family: "payments", aliases: ["fintech", "fin tech", "financial technology", "payments", "payment systems", "payment services", "money transmission", "money transmitters", "neobanks", "open banking", "embedded finance"] },
	{ name: "Digital Assets & Blockchain", family: "digitalAssets", aliases: ["crypto", "cryptocurrency", "cryptocurrencies", "digital assets", "blockchain", "web3", "DeFi", "tokens", "token offerings", "NFTs", "virtual currency", "stablecoins", "DLT"] },
	{ name: "Insurance & Reinsurance", family: "insuranceRegulatory", aliases: ["insurance", "reinsurance", "insurance regulatory", "insurance regulation", "insurance transactions", "insurtech", "captives", "captive insurance", "life insurance", "annuities"] },
	{ name: "Insurance Coverage", family: "insurance", aliases: ["coverage", "coverage litigation", "insurance litigation", "policyholder", "policyholder coverage", "insurance recovery", "D&O coverage", "bad faith"] },

	// Disputes.
	{ name: "Litigation", family: "litigation", aliases: ["disputes", "dispute resolution", "litigation and dispute resolution", "trial", "trials", "trial practice", "civil litigation", "complex litigation", "contentious", "disputes and litigation", "litigator", "general litigation", "courts"] },
	{ name: "Commercial Litigation", family: "litigation", aliases: ["business litigation", "commercial disputes", "contract disputes", "business disputes", "corporate litigation", "shareholder disputes"] },
	{ name: "Appellate", family: "appellate", aliases: ["appeals", "appellate litigation", "Supreme Court", "Supreme Court and appellate", "appellate advocacy"] },
	{ name: "Class Actions", family: "classActions", aliases: ["class action", "class action defense", "mass actions", "collective actions", "consumer class actions"] },
	{ name: "Product Liability & Mass Torts", family: "productLiability", aliases: ["product liability", "products liability", "mass torts", "mass tort", "toxic torts", "toxic tort", "MDL", "MDLs", "torts", "tort", "consumer products", "CPSC", "product safety"] },
	{ name: "Securities Litigation", family: "securitiesLitigation", aliases: ["securities class actions", "shareholder litigation", "derivative litigation", "derivative suits", "M&A litigation", "Delaware litigation"] },
	{ name: "Professional Liability", family: "professionalLiability", aliases: ["professional negligence", "legal malpractice", "accountants liability", "lawyers liability", "D&O liability"] },
	{ name: "Real Estate Litigation", family: "realEstateLitigation", aliases: ["property litigation", "property disputes", "title disputes"] },
	{ name: "Civil Rights", family: "civilRights", aliases: ["civil liberties", "section 1983", "police misconduct", "constitutional torts", "voting rights", "disability rights", "fair housing", "ADA", "accessibility"] },
	{ name: "Consumer Protection", family: "consumerProtection", aliases: ["consumer law", "consumer", "UDAP", "TCPA", "FTC", "lemon law"] },
	{ name: "Legal Ethics & Professional Responsibility", family: "ethics", aliases: ["legal ethics", "professional responsibility", "law firm risk", "attorney ethics", "lawyer discipline"] },
	{ name: "General Practice", family: "litigation", aliases: ["general", "solo practice", "general law"] },
	{ name: "Pro Bono", family: "proBono", aliases: ["public interest", "legal aid", "public interest law", "impact litigation"] },
	{ name: "Animal Law", family: "animal", aliases: ["animal rights", "animal welfare"] },
	{ name: "International Arbitration", family: "arbitration", aliases: ["arbitration", "ICA", "commercial arbitration", "international commercial arbitration", "ICC arbitration", "LCIA", "ICDR", "SIAC", "AAA arbitration", "arbitrations"] },
	{ name: "Investment Treaty Arbitration", family: "investmentTreaty", aliases: ["investment arbitration", "investor-state", "ISDS", "treaty arbitration", "ICSID", "BIT arbitration"] },
	{ name: "Mediation & ADR", family: "mediation", aliases: ["mediation", "ADR", "alternative dispute resolution", "mediator", "conciliation"] },
	{ name: "Public International Law", family: "investmentTreaty", aliases: ["international law", "PIL", "sovereign disputes", "state immunity", "sovereign immunity", "treaties", "law of the sea", "boundary disputes", "international organizations"] },
	{ name: "White Collar & Investigations", family: "investigation", aliases: ["white collar", "white collar defense", "white collar crime", "investigations", "government investigations", "internal investigations", "regulatory investigations", "corporate investigations", "enforcement", "government enforcement", "SEC enforcement", "DOJ investigations", "corporate crime", "business crime", "monitorships", "investigations and white collar", "securities enforcement", "FINRA enforcement"] },
	{ name: "Anti-Corruption & FCPA", family: "whiteCollar", aliases: ["anti-corruption", "anticorruption", "FCPA", "bribery", "anti-bribery", "antibribery", "ABC", "UK Bribery Act", "corruption"] },
	{ name: "Qui Tam & False Claims Act", family: "quiTam", aliases: ["False Claims Act", "FCA", "qui tam", "whistleblower", "whistleblowers", "whistleblower representation"] },
	{ name: "Fraud & Asset Recovery", family: "fraud", aliases: ["fraud", "civil fraud", "asset recovery", "asset tracing", "judgment enforcement", "enforcement of judgments"] },
	{ name: "Corporate Compliance", family: "compliance", aliases: ["compliance", "regulatory compliance", "ethics and compliance", "compliance programs", "compliance and ethics"] },
	{ name: "Crisis Management", family: "crisis", aliases: ["crisis", "crisis response"] },
	{ name: "Criminal Defense", family: "criminal", aliases: ["criminal", "criminal law", "criminal defence", "criminal justice", "crime", "federal criminal defense", "state criminal defense", "DUI", "DWI", "OWI", "drunk driving", "drug crimes", "drug offenses", "violent crimes", "sex crimes", "felonies", "misdemeanors", "expungement", "post-conviction", "habeas", "habeas corpus", "juvenile", "juvenile defense", "juvenile law", "traffic", "traffic offenses", "criminal appeals"] },
	{ name: "Military Law", family: "military", aliases: ["military justice", "court-martial", "courts-martial", "UCMJ"] },

	// People at work.
	{ name: "Labor & Employment", family: "employment", aliases: ["employment", "labor", "labour", "L&E", "employment law", "labor law", "labour law", "employment and labor", "workplace", "workplace law", "HR", "human resources", "employment litigation", "employment counseling", "wrongful termination", "discrimination", "harassment", "workplace discrimination", "wage and hour", "FLSA", "labor relations", "union", "unions", "NLRA", "NLRB", "traditional labor", "restrictive covenants", "non-competes", "noncompetes", "employee mobility", "workplace investigations"] },
	{ name: "Employee Benefits & Executive Compensation", family: "benefits", aliases: ["employee benefits", "benefits", "ERISA", "executive compensation", "exec comp", "compensation", "compensation and benefits", "EBEC", "pensions", "pension", "retirement plans", "401(k)", "401k", "equity compensation", "incentive plans", "health and welfare plans"] },
	{ name: "Workplace Safety", family: "workplaceSafety", aliases: ["OSHA", "occupational safety", "occupational health and safety", "health and safety", "OHS", "MSHA", "mine safety"] },
	{ name: "Immigration", family: "immigration", aliases: ["immigration law", "business immigration", "corporate immigration", "employment-based immigration", "family immigration", "family-based immigration", "visas", "H-1B", "L-1", "O-1", "E-2", "EB-5", "green cards", "naturalization", "citizenship", "asylum", "refugees", "refugee law", "deportation", "removal defense", "global mobility", "work permits", "PERM", "DACA"] },

	// Tax and private wealth.
	{ name: "Tax", family: "tax", aliases: ["taxes", "taxation", "tax law", "corporate tax", "federal tax", "federal income tax", "international tax", "cross-border tax", "tax planning", "tax structuring", "transactional tax", "M&A tax", "partnership tax", "tax advisory", "VAT", "indirect tax", "sales tax", "excise tax", "tax credits"] },
	{ name: "State & Local Tax", family: "salt", aliases: ["SALT", "state tax", "local tax", "state and local taxation", "property tax"] },
	{ name: "Tax Controversy", family: "taxControversy", aliases: ["tax disputes", "tax litigation", "IRS audits", "IRS controversy", "tax audits", "tax appeals", "tax enforcement", "criminal tax"] },
	{ name: "Transfer Pricing", family: "taxControversy", aliases: ["TP"] },
	{ name: "Exempt Organizations", family: "exempt", aliases: ["nonprofit", "non-profit", "nonprofits", "nonprofit organizations", "charities", "charity law", "charitable organizations", "tax-exempt organizations", "tax exempt", "foundations", "private foundations", "philanthropy", "501(c)(3)", "NGOs"] },
	{ name: "Trusts & Estates", family: "privateClient", aliases: ["T&E", "trusts", "estates", "estate planning", "wills", "wills and trusts", "wills and estates", "probate", "estate administration", "trust administration", "private client", "private wealth", "wealth planning", "wealth management", "family office", "family offices", "succession planning", "inheritance", "estate and gift tax", "estate tax", "gift tax", "fiduciary", "fiduciary law", "high net worth", "HNW", "UHNW"] },
	{ name: "Elder Law", family: "elder", aliases: ["elder care", "Medicaid planning", "long-term care planning", "guardianship", "guardianships", "conservatorship", "conservatorships", "special needs planning", "special needs trusts", "elder abuse"] },
	{ name: "Trust & Estate Litigation", family: "estateLitigation", aliases: ["fiduciary litigation", "trust litigation", "estate litigation", "probate litigation", "will contests", "contested estates"] },
	{ name: "Family Law", family: "family", aliases: ["family", "divorce", "divorces", "custody", "child custody", "child support", "alimony", "spousal support", "matrimonial", "matrimonial law", "domestic relations", "prenups", "prenup", "prenuptial agreements", "prenuptial", "postnuptial agreements", "marital agreements", "adoption", "adoptions", "paternity", "domestic violence", "separation", "collaborative law", "family mediation", "surrogacy", "assisted reproduction", "reproductive technology"] },

	// Property and building.
	{ name: "Real Estate", family: "realEstate", aliases: ["RE", "property", "property law", "real property", "commercial real estate", "CRE", "real estate transactions", "real estate development", "development", "commercial property", "conveyancing", "leasing", "commercial leasing", "office leasing", "retail leasing", "real estate acquisitions", "REITs", "REIT", "hospitality", "hotels", "hotels and hospitality", "hospitality and leisure", "title", "title insurance", "1031 exchanges", "condominiums", "residential real estate", "affordable housing", "multifamily"] },
	{ name: "Land Use & Zoning", family: "landUse", aliases: ["land use", "zoning", "planning", "planning and zoning", "entitlements", "land use planning", "eminent domain", "condemnation"] },
	{ name: "Real Estate Finance", family: "realEstateFinance", aliases: ["real estate lending", "mortgage lending", "mortgage finance", "commercial mortgage lending", "mezzanine finance", "mezz", "construction lending", "real estate capital markets"] },
	{ name: "Landlord & Tenant", family: "leasing", aliases: ["landlord tenant", "landlord", "tenant", "eviction", "evictions", "residential leasing", "tenant rights", "tenants rights", "housing", "housing law"] },
	{ name: "Construction", family: "construction", aliases: ["construction law", "engineering and construction", "construction and engineering", "E&C", "construction contracts", "construction disputes", "construction litigation", "construction defects", "construction defect", "mechanics liens", "FIDIC", "EPC", "EPC contracts", "design and construction", "building contracts", "surety", "surety bonds"] },
	{ name: "Infrastructure", family: "infrastructure", aliases: ["infra", "PPP", "P3", "P3s", "public-private partnerships", "projects", "projects and infrastructure", "transport infrastructure", "social infrastructure", "digital infrastructure", "data centers"] },

	// Ideas, technology and media.
	{ name: "Intellectual Property", family: "ip", aliases: ["IP", "IP law", "IP litigation", "intellectual property litigation", "IP transactions", "IP licensing", "IP prosecution", "IP strategy", "brand protection", "anti-counterfeiting", "anticounterfeiting", "IPR"] },
	{ name: "Patents", family: "ip", aliases: ["patent", "patent law", "patent prosecution", "patent litigation", "patent counseling", "PTAB", "Section 337", "ITC litigation", "patent portfolio", "Hatch-Waxman", "ANDA litigation", "biosimilars"] },
	{ name: "Trademarks & Copyright", family: "trademarks", aliases: ["trademarks", "trademark", "TM", "trademark law", "copyright", "copyrights", "copyright law", "trademark and copyright", "brands", "branding", "trade dress", "domain names", "design rights", "TTAB", "DMCA", "Lanham Act", "unfair competition"] },
	{ name: "Trade Secrets", family: "tradeSecrets", aliases: ["trade secret", "trade secret litigation", "confidential information", "DTSA"] },
	{ name: "Technology Transactions", family: "technology", aliases: ["technology", "tech", "tech transactions", "technology law", "technology and commercial transactions", "IT", "information technology", "IT law", "outsourcing", "software", "software licensing", "licensing", "SaaS", "cloud", "cloud computing", "e-commerce", "internet", "internet law", "digital", "digital transformation", "tech licensing", "emerging technologies"] },
	{ name: "Artificial Intelligence", family: "ai", aliases: ["AI", "AI law", "AI governance", "AI regulation", "machine learning", "ML", "generative AI", "GenAI", "AI and machine learning", "algorithms", "autonomous systems"] },
	{ name: "Technology, Media & Telecommunications", family: "technology", aliases: ["TMT", "technology media and telecom", "technology media telecoms"] },
	{ name: "Telecommunications", family: "telecom", aliases: ["telecom", "telecoms", "telecommunications law", "communications", "communications law", "FCC", "spectrum", "wireless", "broadband"] },
	{ name: "Privacy & Data Security", family: "privacy", aliases: ["privacy", "data privacy", "data protection", "data security", "privacy and cybersecurity", "cybersecurity", "cyber security", "cyber", "GDPR", "CCPA", "CPRA", "HIPAA privacy", "data breach", "data breaches", "breach response", "incident response", "information governance", "privacy law", "data law", "data", "COPPA", "BIPA", "biometric privacy", "data governance"] },
	{ name: "Media & Entertainment", family: "media", aliases: ["media", "entertainment", "entertainment law", "film", "film and television", "film and TV", "television", "TV", "music", "music law", "publishing", "broadcasting", "streaming", "theater", "theatre", "talent", "content", "digital media", "video games", "esports", "interactive entertainment", "celebrity", "live events"] },
	{ name: "Defamation & Media Law", family: "defamation", aliases: ["defamation", "libel", "slander", "media litigation", "reputation management", "First Amendment"] },
	{ name: "Sports", family: "sports", aliases: ["sports law", "sport", "sports and entertainment", "athletes", "leagues", "sports teams", "NIL"] },
	{ name: "Advertising & Marketing", family: "advertising", aliases: ["advertising", "marketing", "advertising law", "false advertising", "promotions", "sweepstakes", "influencer marketing", "consumer marketing", "NAD"] },
	{ name: "Fashion & Luxury", family: "fashion", aliases: ["fashion", "fashion law", "luxury", "luxury goods", "beauty", "cosmetics"] },
	{ name: "Art & Cultural Property", family: "art", aliases: ["art", "art law", "cultural property", "museums", "art market", "collectors"] },

	// Regulation and government.
	{ name: "Antitrust & Competition", family: "antitrust", aliases: ["antitrust", "competition", "competition law", "antitrust law", "competition and antitrust", "merger control", "merger clearance", "merger review", "HSR", "Hart-Scott-Rodino", "cartels", "cartel", "cartel defense", "price fixing", "monopolization", "abuse of dominance", "antitrust litigation", "competition litigation", "state aid", "foreign subsidies"] },
	{ name: "International Trade & Sanctions", family: "trade", aliases: ["international trade", "trade", "sanctions", "export controls", "export control", "ITAR", "EAR", "OFAC", "customs", "customs and trade", "import", "imports", "tariffs", "trade remedies", "anti-dumping", "antidumping", "countervailing duties", "WTO", "trade compliance", "export compliance", "trade and sanctions", "economic sanctions", "CFIUS", "FDI", "FDI screening", "foreign investment", "foreign direct investment", "national security", "national security reviews", "forced labor", "UFLPA", "trade policy"] },
	{ name: "Energy", family: "energy", aliases: ["energy law", "power", "electricity", "utilities", "utility", "energy regulatory", "FERC", "energy and infrastructure", "energy and resources", "energy transition", "nuclear", "nuclear energy", "hydrogen", "energy storage", "batteries", "carbon capture", "CCUS"] },
	{ name: "Oil & Gas", family: "oilGas", aliases: ["O&G", "oil", "gas", "upstream", "midstream", "downstream", "LNG", "pipelines", "petroleum", "oilfield services"] },
	{ name: "Renewable Energy", family: "renewable", aliases: ["renewables", "solar", "wind", "offshore wind", "clean energy", "green energy", "geothermal", "hydropower", "hydro", "PPAs", "PPA", "power purchase agreements", "community solar", "tax equity"] },
	{ name: "Mining & Natural Resources", family: "mining", aliases: ["mining", "mining law", "natural resources", "minerals", "metals", "metals and mining", "resources", "critical minerals", "coal", "quarrying"] },
	{ name: "Environmental", family: "environmental", aliases: ["environment", "environmental law", "environmental regulatory", "environmental compliance", "environmental litigation", "CERCLA", "Superfund", "Clean Air Act", "Clean Water Act", "RCRA", "NEPA", "environmental permitting", "permitting", "remediation", "contamination", "toxic substances", "TSCA", "PFAS", "water law", "water rights", "wildlife", "endangered species"] },
	{ name: "ESG & Climate", family: "esg", aliases: ["ESG", "climate", "climate change", "sustainability", "sustainable finance", "green finance", "ESG and sustainability", "climate and sustainability", "carbon markets", "carbon", "emissions", "emissions trading", "net zero", "climate risk", "ESG reporting", "CSRD"] },
	{ name: "Agriculture", family: "agriculture", aliases: ["agricultural law", "agribusiness", "farming", "farms", "agtech", "food and agriculture"] },
	{ name: "Health Care", family: "healthcare", aliases: ["healthcare", "health", "health law", "health care law", "healthcare law", "hospitals", "health systems", "health care regulatory", "healthcare regulatory", "HIPAA", "Stark", "Stark Law", "anti-kickback", "AKS", "Medicare", "Medicaid", "digital health", "telehealth", "telemedicine", "health care transactions", "physician practices", "behavioral health", "long-term care", "senior living", "managed care", "payors"] },
	{ name: "Life Sciences", family: "lifeSciences", aliases: ["pharma", "pharmaceutical", "pharmaceuticals", "pharmaceutical law", "biotech", "biotechnology", "medical devices", "medical device", "medtech", "devices", "diagnostics", "FDA", "FDA regulatory", "food and drug", "food and drug law", "clinical trials", "drug development", "biologics", "drug pricing", "life sciences regulatory"] },
	{ name: "Food & Beverage", family: "food", aliases: ["food", "food law", "F&B", "beverage", "food safety", "FSMA", "alcohol", "alcohol beverage", "alcoholic beverages", "wine", "beer", "spirits", "dietary supplements", "supplements"] },
	{ name: "Cannabis", family: "cannabis", aliases: ["marijuana", "hemp", "CBD", "cannabis law", "psychedelics"] },
	{ name: "Gaming", family: "gaming", aliases: ["gambling", "casinos", "casino", "sports betting", "betting", "iGaming", "online gaming", "lotteries", "lottery", "gaming law", "tribal gaming"] },
	{ name: "Government Contracts", family: "government", aliases: ["govcon", "government contracting", "federal contracts", "federal contracting", "public procurement", "procurement", "public contracts", "bid protests", "bid protest", "FAR", "DFARS", "GSA", "defense contracting", "aerospace and defense", "A&D", "defense industry"] },
	{ name: "Government & Public Policy", family: "publicPolicy", aliases: ["government relations", "government affairs", "public policy", "lobbying", "legislative", "legislative affairs", "political law", "election law", "elections", "campaign finance", "government ethics", "public affairs"] },
	{ name: "Administrative Law", family: "administrative", aliases: ["administrative", "admin law", "agency practice", "rulemaking", "APA", "public law", "regulatory litigation", "judicial review"] },
	{ name: "Constitutional Law", family: "constitutional", aliases: ["constitutional", "constitutional litigation", "con law", "Second Amendment"] },
	{ name: "Municipal Law", family: "municipal", aliases: ["municipal", "local government", "local government law", "municipalities", "public entities", "school districts"] },
	{ name: "Education Law", family: "education", aliases: ["education", "higher education", "school law", "schools", "universities", "Title IX", "special education", "student discipline", "colleges and universities"] },
	{ name: "Native American Law", family: "nativeAmerican", aliases: ["Indian law", "federal Indian law", "tribal law", "tribal", "tribes", "tribal nations"] },
	{ name: "Veterans Law", family: "veterans", aliases: ["veterans", "veterans benefits", "VA benefits", "VA claims"] },
	{ name: "Human Rights", family: "humanRights", aliases: ["business and human rights", "international human rights", "modern slavery"] },

	// People and their injuries.
	{ name: "Personal Injury", family: "personalInjury", aliases: ["PI", "injury", "injury law", "accidents", "accident", "car accidents", "auto accidents", "motor vehicle accidents", "truck accidents", "trucking accidents", "motorcycle accidents", "slip and fall", "premises liability", "wrongful death", "catastrophic injury", "catastrophic injuries", "brain injury", "spinal cord injury", "dog bites", "plaintiff", "plaintiffs", "plaintiff-side"] },
	{ name: "Medical Malpractice", family: "medMal", aliases: ["med mal", "medmal", "medical negligence", "clinical negligence", "birth injury", "birth injuries", "nursing home abuse", "nursing home negligence", "malpractice"] },
	{ name: "Workers' Compensation", family: "workersComp", aliases: ["workers comp", "workman's comp", "workmans comp", "work comp", "workplace injury", "workplace injuries", "occupational injury"] },
	{ name: "Social Security Disability", family: "ssdi", aliases: ["SSDI", "SSI", "disability benefits", "social security", "disability claims", "long-term disability", "LTD"] },

	// Ships, planes and freight.
	{ name: "Admiralty & Maritime", family: "maritime", aliases: ["admiralty", "maritime", "maritime law", "admiralty law", "shipping", "shipping law", "marine", "marine insurance", "charter parties", "charterparties", "ports", "vessels", "cargo claims", "cargo", "ship arrest", "Jones Act", "cruise lines", "offshore"] },
	{ name: "Aviation & Aerospace", family: "aviation", aliases: ["aviation", "aviation law", "aerospace", "airlines", "airline", "aircraft", "air transport", "drones", "UAS", "airports", "space", "space law", "satellites", "satellite"] },
	{ name: "Transportation & Logistics", family: "logistics", aliases: ["transportation", "transport", "logistics", "trucking", "freight", "rail", "railroads", "railways", "railroad", "automotive", "auto", "motor vehicles", "autonomous vehicles", "mobility", "carriers", "transportation law"] },
];

// Words that say nothing of which practice it is.
const NOISE = new Set(["and", "the", "of", "law", "laws", "practice", "practices", "group", "groups", "department", "dept", "team", "area", "areas"]);

// One word, in the form it is compared in: "Acquisitions" and "acquisition"
// are one word, as are "taxes" and "tax".
function stem(word) {
	if (word.length > 4 && word.endsWith("ies")) return `${word.slice(0, -3)}y`;
	if (word.length > 4 && /(xes|ches|shes|sses)$/.test(word)) return word.slice(0, -2);
	if (word.length > 3 && word.endsWith("s") && /(ss|us|is|os)$/.test(word) == false) return word.slice(0, -1);
	return word;
}

// The words of what a member typed: lower case, without accents, apostrophes
// or punctuation, "&" and "+" read as "and", and the noise left out.
function wordsOf(text) {
	return String(text ?? "")
		.normalize("NFKD").replace(/[̀-ͯ]/g, "")
		.toLowerCase()
		.replace(/[’'`]/g, "")
		.replace(/[&+]/g, " and ")
		.split(/[^a-z0-9]+/)
		.filter(word => word != "" && NOISE.has(word) == false)
		.map(stem);
}

const keyOf = text => wordsOf(text).join("");

// Every way of writing each practice, by its key: "M&A", "M and A" and "MA"
// are one key.
const BY_KEY = new Map();
const FORMS = [];
for (const practice of PRACTICES) {
	for (const form of [practice.name, ...practice.aliases]) {
		const words = wordsOf(form);
		const key = words.join("");
		if (key == "" || BY_KEY.has(key)) continue;
		BY_KEY.set(key, practice);
		FORMS.push({ words, practice });
	}
}

// Edits between two words (Damerau-Levenshtein, adjacent swaps counted once).
function distance(a, b) {
	const rows = Array.from({ length: a.length + 1 }, (_, i) => Array.from({ length: b.length + 1 }, (_, j) => (i == 0 ? j : j == 0 ? i : 0)));
	for (let i = 1; i <= a.length; i += 1) {
		for (let j = 1; j <= b.length; j += 1) {
			const cost = a[i - 1] == b[j - 1] ? 0 : 1;
			rows[i][j] = Math.min(rows[i - 1][j] + 1, rows[i][j - 1] + 1, rows[i - 1][j - 1] + cost);
			if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) rows[i][j] = Math.min(rows[i][j], rows[i - 2][j - 2] + 1);
		}
	}
	return rows[a.length][b.length];
}

// How many slips a word of this length may carry and still be recognised:
// none in a short word, where one letter makes another word.
const slipsAllowed = length => (length < 5 ? 0 : length < 9 ? 1 : 2);

// A misspelt practice ("Merges and Aqusition"): the same number of words,
// each starting with the same letter and within its few slips, and one
// practice nearest, never a tie. "Prime" is not "crime".
function nearest(words) {
	let best = null;
	let bestDistance = Infinity;
	let tied = false;
	for (const form of FORMS) {
		if (form.words.length != words.length) continue;
		let total = 0;
		for (let index = 0; index < words.length && total != Infinity; index += 1) {
			const typed = words[index];
			const known = form.words[index];
			if (typed[0] != known[0]) { total = Infinity; break; }
			const slips = distance(typed, known);
			total = slips <= slipsAllowed(Math.max(typed.length, known.length)) ? total + slips : Infinity;
		}
		if (total == Infinity) continue;
		if (total < bestDistance) { best = form.practice; bestDistance = total; tied = false; }
		else if (total == bestDistance && form.practice != best) tied = true;
	}
	return tied ? null : best;
}

// The practice one piece of text names, or null.
export function practiceOf(text) {
	const words = wordsOf(text);
	if (words.length == 0) return null;
	return BY_KEY.get(words.join("")) ?? nearest(words);
}

// The pieces a member listed: "M&A, PE", "Tax; Benefits", "Banking/Finance".
// "Tax and Employment" is two practices only when both are known; "Labor and
// Employment" is one.
function piecesOf(text) {
	const pieces = text.split(/\s*[,;/|\n]\s*/).map(piece => piece.trim()).filter(piece => piece != "");
	return pieces.flatMap(piece => {
		if (practiceOf(piece) != null) return [piece];
		const parts = piece.split(/\s+and\s+|\s*&\s*|\s*\+\s*/i).map(part => part.trim()).filter(part => part != "");
		return parts.length > 1 && parts.every(part => practiceOf(part) != null) ? parts : [piece];
	});
}

// What the practice field says once the member leaves it: each piece by its
// official name, a piece no practice matches as it was typed, and nothing at
// all changed when nothing matches.
export function normalizePractice(text) {
	const typed = String(text ?? "");
	if (typed.trim() == "") return typed;
	const pieces = piecesOf(typed);
	const named = pieces.map(piece => practiceOf(piece)?.name ?? piece);
	if (named.every((name, index) => name == pieces[index])) return typed;
	return [...new Set(named)].join(", ");
}

// The family of work a practice belongs to, for the demonstration: the first
// piece's that is known, or null.
export function familyOf(text) {
	for (const piece of piecesOf(String(text ?? ""))) {
		const practice = practiceOf(piece);
		if (practice != null) return practice.family;
	}
	return null;
}
