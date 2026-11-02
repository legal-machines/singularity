// The countries and territories an organisation can be registered in, for the
// onboarding's Country field (the owner, 2026-10-29): every UN M49 "Country or
// Area", and Taiwan and Kosovo, where organisations are incorporated too
// though M49 lists neither. Each has its ISO 3166 code, so "US" finds the
// United States, and the short English name the field shows and the
// registration sends; the M49 name and a few common other names are searched
// too. The list and its ranking come from the Alpha onboarding's country
// combobox (git history: code/frontend/src/entities/country, before 25b02afc7).

// [M49 name, ISO 3166-1 alpha-2 code, the name shown where it differs]
const ENTRIES = [
	["Afghanistan", "AF"],
	["Åland Islands", "AX"],
	["Albania", "AL"],
	["Algeria", "DZ"],
	["American Samoa", "AS"],
	["Andorra", "AD"],
	["Angola", "AO"],
	["Anguilla", "AI"],
	["Antarctica", "AQ"],
	["Antigua and Barbuda", "AG"],
	["Argentina", "AR"],
	["Armenia", "AM"],
	["Aruba", "AW"],
	["Australia", "AU"],
	["Austria", "AT"],
	["Azerbaijan", "AZ"],
	["Bahamas", "BS"],
	["Bahrain", "BH"],
	["Bangladesh", "BD"],
	["Barbados", "BB"],
	["Belarus", "BY"],
	["Belgium", "BE"],
	["Belize", "BZ"],
	["Benin", "BJ"],
	["Bermuda", "BM"],
	["Bhutan", "BT"],
	["Bolivia (Plurinational State of)", "BO", "Bolivia"],
	["Bonaire, Sint Eustatius and Saba", "BQ"],
	["Bosnia and Herzegovina", "BA"],
	["Botswana", "BW"],
	["Bouvet Island", "BV"],
	["Brazil", "BR"],
	["British Indian Ocean Territory", "IO"],
	["British Virgin Islands", "VG"],
	["Brunei Darussalam", "BN", "Brunei"],
	["Bulgaria", "BG"],
	["Burkina Faso", "BF"],
	["Burundi", "BI"],
	["Cabo Verde", "CV"],
	["Cambodia", "KH"],
	["Cameroon", "CM"],
	["Canada", "CA"],
	["Cayman Islands", "KY"],
	["Central African Republic", "CF"],
	["Chad", "TD"],
	["Chile", "CL"],
	["China", "CN"],
	["China, Hong Kong Special Administrative Region", "HK", "Hong Kong"],
	["China, Macao Special Administrative Region", "MO", "Macau"],
	["Christmas Island", "CX"],
	["Cocos (Keeling) Islands", "CC"],
	["Colombia", "CO"],
	["Comoros", "KM"],
	["Congo", "CG", "Republic of the Congo"],
	["Cook Islands", "CK"],
	["Costa Rica", "CR"],
	["Côte d’Ivoire", "CI"],
	["Croatia", "HR"],
	["Cuba", "CU"],
	["Curaçao", "CW"],
	["Cyprus", "CY"],
	["Czechia", "CZ"],
	["Democratic People's Republic of Korea", "KP", "North Korea"],
	["Democratic Republic of the Congo", "CD"],
	["Denmark", "DK"],
	["Djibouti", "DJ"],
	["Dominica", "DM"],
	["Dominican Republic", "DO"],
	["Ecuador", "EC"],
	["Egypt", "EG"],
	["El Salvador", "SV"],
	["Equatorial Guinea", "GQ"],
	["Eritrea", "ER"],
	["Estonia", "EE"],
	["Eswatini", "SZ"],
	["Ethiopia", "ET"],
	["Falkland Islands (Malvinas)", "FK"],
	["Faroe Islands", "FO"],
	["Fiji", "FJ"],
	["Finland", "FI"],
	["France", "FR"],
	["French Guiana", "GF"],
	["French Polynesia", "PF"],
	["French Southern Territories", "TF"],
	["Gabon", "GA"],
	["Gambia", "GM"],
	["Georgia", "GE"],
	["Germany", "DE"],
	["Ghana", "GH"],
	["Gibraltar", "GI"],
	["Greece", "GR"],
	["Greenland", "GL"],
	["Grenada", "GD"],
	["Guadeloupe", "GP"],
	["Guam", "GU"],
	["Guatemala", "GT"],
	["Guernsey", "GG"],
	["Guinea", "GN"],
	["Guinea-Bissau", "GW"],
	["Guyana", "GY"],
	["Haiti", "HT"],
	["Heard Island and McDonald Islands", "HM"],
	["Holy See", "VA", "Vatican City (Holy See)"],
	["Honduras", "HN"],
	["Hungary", "HU"],
	["Iceland", "IS"],
	["India", "IN"],
	["Indonesia", "ID"],
	["Iran (Islamic Republic of)", "IR", "Iran"],
	["Iraq", "IQ"],
	["Ireland", "IE"],
	["Isle of Man", "IM"],
	["Israel", "IL"],
	["Italy", "IT"],
	["Jamaica", "JM"],
	["Japan", "JP"],
	["Jersey", "JE"],
	["Jordan", "JO"],
	["Kazakhstan", "KZ"],
	["Kenya", "KE"],
	["Kiribati", "KI"],
	["Kosovo", "XK"],
	["Kuwait", "KW"],
	["Kyrgyzstan", "KG"],
	["Lao People's Democratic Republic", "LA", "Laos"],
	["Latvia", "LV"],
	["Lebanon", "LB"],
	["Lesotho", "LS"],
	["Liberia", "LR"],
	["Libya", "LY"],
	["Liechtenstein", "LI"],
	["Lithuania", "LT"],
	["Luxembourg", "LU"],
	["Madagascar", "MG"],
	["Malawi", "MW"],
	["Malaysia", "MY"],
	["Maldives", "MV"],
	["Mali", "ML"],
	["Malta", "MT"],
	["Marshall Islands", "MH"],
	["Martinique", "MQ"],
	["Mauritania", "MR"],
	["Mauritius", "MU"],
	["Mayotte", "YT"],
	["Mexico", "MX"],
	["Micronesia (Federated States of)", "FM", "Micronesia"],
	["Monaco", "MC"],
	["Mongolia", "MN"],
	["Montenegro", "ME"],
	["Montserrat", "MS"],
	["Morocco", "MA"],
	["Mozambique", "MZ"],
	["Myanmar", "MM"],
	["Namibia", "NA"],
	["Nauru", "NR"],
	["Nepal", "NP"],
	["Netherlands (Kingdom of the)", "NL", "Netherlands"],
	["New Caledonia", "NC"],
	["New Zealand", "NZ"],
	["Nicaragua", "NI"],
	["Niger", "NE"],
	["Nigeria", "NG"],
	["Niue", "NU"],
	["Norfolk Island", "NF"],
	["North Macedonia", "MK"],
	["Northern Mariana Islands", "MP"],
	["Norway", "NO"],
	["Oman", "OM"],
	["Pakistan", "PK"],
	["Palau", "PW"],
	["Panama", "PA"],
	["Papua New Guinea", "PG"],
	["Paraguay", "PY"],
	["Peru", "PE"],
	["Philippines", "PH"],
	["Pitcairn", "PN"],
	["Poland", "PL"],
	["Portugal", "PT"],
	["Puerto Rico", "PR"],
	["Qatar", "QA"],
	["Republic of Korea", "KR", "South Korea"],
	["Republic of Moldova", "MD", "Moldova"],
	["Réunion", "RE"],
	["Romania", "RO"],
	["Russian Federation", "RU", "Russia"],
	["Rwanda", "RW"],
	["Saint Barthélemy", "BL"],
	["Saint Helena", "SH"],
	["Saint Kitts and Nevis", "KN"],
	["Saint Lucia", "LC"],
	["Saint Martin (French Part)", "MF"],
	["Saint Pierre and Miquelon", "PM"],
	["Saint Vincent and the Grenadines", "VC"],
	["Samoa", "WS"],
	["San Marino", "SM"],
	["Sao Tome and Principe", "ST", "São Tomé and Príncipe"],
	["Saudi Arabia", "SA"],
	["Senegal", "SN"],
	["Serbia", "RS"],
	["Seychelles", "SC"],
	["Sierra Leone", "SL"],
	["Singapore", "SG"],
	["Sint Maarten (Dutch part)", "SX"],
	["Slovakia", "SK"],
	["Slovenia", "SI"],
	["Solomon Islands", "SB"],
	["Somalia", "SO"],
	["South Africa", "ZA"],
	["South Georgia and the South Sandwich Islands", "GS"],
	["South Sudan", "SS"],
	["Spain", "ES"],
	["Sri Lanka", "LK"],
	["State of Palestine", "PS", "Palestine"],
	["Sudan", "SD"],
	["Suriname", "SR"],
	["Svalbard and Jan Mayen Islands", "SJ"],
	["Sweden", "SE"],
	["Switzerland", "CH"],
	["Syrian Arab Republic", "SY", "Syria"],
	["Taiwan", "TW"],
	["Tajikistan", "TJ"],
	["Thailand", "TH"],
	["Timor-Leste", "TL"],
	["Togo", "TG"],
	["Tokelau", "TK"],
	["Tonga", "TO"],
	["Trinidad and Tobago", "TT"],
	["Tunisia", "TN"],
	["Türkiye", "TR"],
	["Turkmenistan", "TM"],
	["Turks and Caicos Islands", "TC"],
	["Tuvalu", "TV"],
	["Uganda", "UG"],
	["Ukraine", "UA"],
	["United Arab Emirates", "AE"],
	["United Kingdom of Great Britain and Northern Ireland", "GB", "United Kingdom"],
	["United Republic of Tanzania", "TZ", "Tanzania"],
	["United States Minor Outlying Islands", "UM", "U.S. Minor Outlying Islands"],
	["United States of America", "US", "United States"],
	["United States Virgin Islands", "VI", "U.S. Virgin Islands"],
	["Uruguay", "UY"],
	["Uzbekistan", "UZ"],
	["Vanuatu", "VU"],
	["Venezuela (Bolivarian Republic of)", "VE", "Venezuela"],
	["Viet Nam", "VN", "Vietnam"],
	["Wallis and Futuna Islands", "WF"],
	["Western Sahara", "EH"],
	["Yemen", "YE"],
	["Zambia", "ZM"],
	["Zimbabwe", "ZW"],
];

// Each country's ISO 3166-1 alpha-3 code ("DEU" finds Germany). Kosovo's XKX is
// the code the EU and others use, as ISO assigns Kosovo none.
const ALPHA3 = Object.fromEntries(`
	AF:AFG AX:ALA AL:ALB DZ:DZA AS:ASM AD:AND AO:AGO AI:AIA AQ:ATA AG:ATG AR:ARG AM:ARM AW:ABW AU:AUS
	AT:AUT AZ:AZE BS:BHS BH:BHR BD:BGD BB:BRB BY:BLR BE:BEL BZ:BLZ BJ:BEN BM:BMU BT:BTN BO:BOL BQ:BES
	BA:BIH BW:BWA BV:BVT BR:BRA IO:IOT VG:VGB BN:BRN BG:BGR BF:BFA BI:BDI CV:CPV KH:KHM CM:CMR CA:CAN
	KY:CYM CF:CAF TD:TCD CL:CHL CN:CHN HK:HKG MO:MAC CX:CXR CC:CCK CO:COL KM:COM CG:COG CK:COK CR:CRI
	CI:CIV HR:HRV CU:CUB CW:CUW CY:CYP CZ:CZE KP:PRK CD:COD DK:DNK DJ:DJI DM:DMA DO:DOM EC:ECU EG:EGY
	SV:SLV GQ:GNQ ER:ERI EE:EST SZ:SWZ ET:ETH FK:FLK FO:FRO FJ:FJI FI:FIN FR:FRA GF:GUF PF:PYF TF:ATF
	GA:GAB GM:GMB GE:GEO DE:DEU GH:GHA GI:GIB GR:GRC GL:GRL GD:GRD GP:GLP GU:GUM GT:GTM GG:GGY GN:GIN
	GW:GNB GY:GUY HT:HTI HM:HMD VA:VAT HN:HND HU:HUN IS:ISL IN:IND ID:IDN IR:IRN IQ:IRQ IE:IRL IM:IMN
	IL:ISR IT:ITA JM:JAM JP:JPN JE:JEY JO:JOR KZ:KAZ KE:KEN KI:KIR XK:XKX KW:KWT KG:KGZ LA:LAO LV:LVA
	LB:LBN LS:LSO LR:LBR LY:LBY LI:LIE LT:LTU LU:LUX MG:MDG MW:MWI MY:MYS MV:MDV ML:MLI MT:MLT MH:MHL
	MQ:MTQ MR:MRT MU:MUS YT:MYT MX:MEX FM:FSM MC:MCO MN:MNG ME:MNE MS:MSR MA:MAR MZ:MOZ MM:MMR NA:NAM
	NR:NRU NP:NPL NL:NLD NC:NCL NZ:NZL NI:NIC NE:NER NG:NGA NU:NIU NF:NFK MK:MKD MP:MNP NO:NOR OM:OMN
	PK:PAK PW:PLW PA:PAN PG:PNG PY:PRY PE:PER PH:PHL PN:PCN PL:POL PT:PRT PR:PRI QA:QAT KR:KOR MD:MDA
	RE:REU RO:ROU RU:RUS RW:RWA BL:BLM SH:SHN KN:KNA LC:LCA MF:MAF PM:SPM VC:VCT WS:WSM SM:SMR ST:STP
	SA:SAU SN:SEN RS:SRB SC:SYC SL:SLE SG:SGP SX:SXM SK:SVK SI:SVN SB:SLB SO:SOM ZA:ZAF GS:SGS SS:SSD
	ES:ESP LK:LKA PS:PSE SD:SDN SR:SUR SJ:SJM SE:SWE CH:CHE SY:SYR TW:TWN TJ:TJK TH:THA TL:TLS TG:TGO
	TK:TKL TO:TON TT:TTO TN:TUN TR:TUR TM:TKM TC:TCA TV:TUV UG:UGA UA:UKR AE:ARE GB:GBR TZ:TZA UM:UMI
	US:USA VI:VIR UY:URY UZ:UZB VU:VUT VE:VEN VN:VNM WF:WLF EH:ESH YE:YEM ZM:ZMB ZW:ZWE
`.trim().split(/\s+/).map((pair) => pair.split(":")));

// Other names people type for a country, searched but never shown: common
// abbreviations, short and former names, and its own name in its own language.
const ALIASES = {
	US: ["America", "United States of America", "Estados Unidos", "États-Unis", "Vereinigte Staaten", "США", "Соединённые Штаты", "Америка"],
	GB: ["UK", "Britain", "Great Britain", "England", "Scotland", "Wales", "Northern Ireland", "Royaume-Uni", "Vereinigtes Königreich", "Reino Unido", "Великобритания", "Англия"],
	AE: ["UAE", "Emirates", "Dubai", "Abu Dhabi", "الإمارات", "ОАЭ"],
	SA: ["KSA", "Saudi", "السعودية"],
	ZA: ["RSA", "Suid-Afrika", "ЮАР"],
	KR: ["Korea", "ROK", "대한민국", "한국", "Южная Корея"],
	KP: ["DPRK", "조선", "Северная Корея"],
	CN: ["PRC", "People's Republic of China", "Mainland China", "中国", "中华人民共和国", "Китай"],
	TW: ["ROC", "Republic of China", "Chinese Taipei", "台灣", "台湾", "Тайвань"],
	HK: ["Hong Kong SAR", "香港", "Гонконг"],
	MO: ["Macao", "Macao SAR", "澳門", "澳门"],
	JP: ["Nippon", "Nihon", "日本", "Япония"],
	DE: ["Deutschland", "Allemagne", "Alemania", "Germania", "Германия"],
	FR: ["Frankreich", "Francia", "Франция"],
	ES: ["España", "Spanien", "Espagne", "Испания"],
	IT: ["Italia", "Italien", "Италия"],
	PT: ["Португалия"],
	NL: ["Holland", "Nederland", "Niederlande", "Pays-Bas", "Países Bajos", "Нидерланды", "Голландия"],
	BE: ["België", "Belgique", "Belgien", "Бельгия"],
	CH: ["Swiss", "Schweiz", "Suisse", "Svizzera", "Svizra", "Швейцария"],
	AT: ["Österreich", "Autriche", "Австрия"],
	LU: ["Luxemburg", "Lëtzebuerg", "Люксембург"],
	LI: ["Лихтенштейн"],
	IE: ["Éire", "Republic of Ireland", "Ирландия"],
	SE: ["Sverige", "Швеция"],
	NO: ["Norge", "Noreg", "Норвегия"],
	DK: ["Danmark", "Дания"],
	FI: ["Suomi", "Финляндия"],
	IS: ["Ísland", "Исландия"],
	FO: ["Faroes", "Føroyar"],
	GL: ["Kalaallit Nunaat", "Grønland"],
	PL: ["Polska", "Польша"],
	CZ: ["Czech Republic", "Česko", "Česká republika", "Чехия"],
	SK: ["Slovensko", "Словакия"],
	HU: ["Magyarország", "Венгрия"],
	RO: ["România", "Румыния"],
	BG: ["България", "Болгария"],
	GR: ["Hellas", "Ελλάδα", "Ελλάς", "Греция"],
	CY: ["Κύπρος", "Kıbrıs", "Кипр"],
	MT: ["Мальта"],
	HR: ["Hrvatska", "Хорватия"],
	SI: ["Slovenija", "Словения"],
	RS: ["Srbija", "Србија", "Сербия"],
	BA: ["Bosnia", "Herzegovina", "Bosna i Hercegovina", "Босния и Герцеговина"],
	ME: ["Crna Gora", "Црна Гора", "Черногория"],
	MK: ["Macedonia", "FYROM", "Северна Македонија", "Северная Македония"],
	AL: ["Shqipëri", "Shqipëria", "Албания"],
	XK: ["Kosova", "Косово"],
	UA: ["Ukraina", "Україна", "Украина"],
	BY: ["Belorussia", "Byelorussia", "Беларусь", "Белоруссия"],
	RU: ["Rossiya", "Россия", "Российская Федерация", "РФ"],
	MD: ["Молдова", "Молдавия"],
	LT: ["Lietuva", "Литва"],
	LV: ["Latvija", "Латвия"],
	EE: ["Eesti", "Эстония"],
	KZ: ["Qazaqstan", "Қазақстан", "Казахстан"],
	UZ: ["Oʻzbekiston", "Узбекистан"],
	KG: ["Kyrgyz Republic", "Kirghizia", "Кыргызстан", "Киргизия"],
	TJ: ["Тоҷикистон", "Таджикистан"],
	TM: ["Türkmenistan", "Туркменистан"],
	AZ: ["Azərbaycan", "Азербайджан"],
	AM: ["Hayastan", "Հայաստան", "Армения"],
	GE: ["Sakartvelo", "საქართველო", "Грузия"],
	TR: ["Turkey", "Турция"],
	IL: ["ישראל", "Израиль"],
	PS: ["فلسطين"],
	JO: ["الأردن"],
	LB: ["لبنان"],
	SY: ["سوريا"],
	IQ: ["العراق"],
	IR: ["Persia", "ایران", "Иран"],
	KW: ["الكويت"],
	BH: ["البحرين"],
	QA: ["قطر"],
	OM: ["عُمان", "عمان"],
	YE: ["اليمن"],
	EG: ["Misr", "مصر", "Египет"],
	MA: ["Maroc", "المغرب"],
	DZ: ["Algérie", "الجزائر"],
	TN: ["Tunisie", "تونس"],
	LY: ["ليبيا"],
	IN: ["Bharat", "Hindustan", "भारत", "Индия"],
	PK: ["پاکستان"],
	BD: ["বাংলাদেশ"],
	LK: ["Ceylon", "ශ්‍රී ලංකාව", "இலங்கை"],
	NP: ["नेपाल"],
	MM: ["Burma", "မြန်မာ"],
	TH: ["Siam", "ประเทศไทย", "Таиланд"],
	VN: ["Вьетнам"],
	LA: ["ລາວ"],
	KH: ["Kampuchea", "កម្ពុជា"],
	SG: ["Singapura", "新加坡", "Сингапур"],
	PH: ["Pilipinas", "Filipinas"],
	TL: ["East Timor"],
	MN: ["Монгол Улс", "Монголия"],
	MX: ["Мексика"],
	BR: ["Brasil", "Бразилия"],
	AR: ["Аргентина"],
	CL: ["Чили"],
	DO: ["República Dominicana", "Dominican"],
	CA: ["Kanada", "Канада"],
	NZ: ["Aotearoa", "Новая Зеландия"],
	AU: ["Австралия"],
	CI: ["Ivory Coast"],
	CD: ["DRC", "DR Congo", "Congo-Kinshasa", "Zaire"],
	CG: ["Congo-Brazzaville"],
	CF: ["CAR"],
	CV: ["Cape Verde"],
	SZ: ["Swaziland"],
	GW: ["Guinea Bissau"],
	PG: ["PNG"],
	VA: ["Vatican", "Vatican City", "Ватикан"],
	KN: ["St Kitts and Nevis", "St Kitts", "Saint Kitts"],
	LC: ["St Lucia"],
	VC: ["St Vincent", "St Vincent and the Grenadines"],
	SH: ["St Helena"],
	PM: ["St Pierre and Miquelon"],
	BL: ["St Barts", "St Barths", "Saint Barts", "St Barthélemy"],
	MF: ["St Martin"],
	SX: ["St Maarten"],
	VG: ["BVI"],
	VI: ["USVI"],
	UM: ["US Minor Outlying Islands"],
	TT: ["Trinidad", "Tobago"],
	AG: ["Antigua", "Barbuda"],
	TC: ["Turks and Caicos", "TCI"],
	BQ: ["Bonaire", "Sint Eustatius", "Saba", "Caribbean Netherlands"],
	FK: ["Falklands", "Malvinas"],
	IO: ["BIOT", "Chagos"],
	GS: ["South Georgia"],
	SJ: ["Svalbard", "Jan Mayen"],
	WF: ["Wallis", "Futuna"],
	PN: ["Pitcairn Islands"],
	CC: ["Cocos Islands", "Keeling Islands"],
	IM: ["Mann"],
};

export const COUNTRIES = Object.freeze(
	ENTRIES.map(([m49, code, label]) => Object.freeze({
		label: label ?? m49, code, m49, aliases: [ALPHA3[code], ...(ALIASES[code] ?? [])].filter(Boolean),
	})).sort((a, b) => a.label.localeCompare(b.label, "en")),
);

// Case, accents, dots and repeated spaces do not count: "U.S.A." is "usa", and
// "Mexico" finds "México".
function normalize(text) {
	return text
		.normalize("NFD")
		.replace(/[\u0300-\u036f]/g, "")
		.replace(/[\u2018\u2019\u02bc]/g, "'")
		.replace(/\./g, "")
		.replace(/\s+/g, " ")
		.toLowerCase()
		.trim();
}

// Derived once from the static catalogue, rather than normalising every
// country and alias again on each keystroke. Shown names win duplicate forms.
const SEARCH_INDEX = COUNTRIES.map((country) => {
	const texts = new Map();
	for (const [name, shown] of [[country.label, true], [country.code, false], ...country.aliases.map((name) => [name, false]), [country.m49, false]]) {
		const text = normalize(name);
		if (!texts.has(text)) texts.set(text, shown);
	}
	return { country, texts: [...texts].map(([text, shown]) => ({ text, shown })) };
});
const EXACT_NAMES = new Map();
for (const { country, texts } of SEARCH_INDEX) {
	for (const { text } of texts) if (!EXACT_NAMES.has(text)) EXACT_NAMES.set(text, country);
}

const isWordCharacter = (character) => /[a-z0-9]/.test(character ?? "");

// The best ordered subsequence of the query's letters in the text: adjacent
// letters count most, small gaps more than large ones, an early start a
// little.
function subsequenceScore(query, text) {
	if (query.length > text.length) return null;
	let scores = new Array(text.length).fill(-Infinity);
	let starts = new Array(text.length).fill(-1);
	for (let index = 0; index < text.length; index += 1) {
		if (text[index] === query[0]) {
			scores[index] = 3;
			starts[index] = index;
		}
	}
	for (let letter = 1; letter < query.length; letter += 1) {
		const nextScores = new Array(text.length).fill(-Infinity);
		const nextStarts = new Array(text.length).fill(-1);
		for (let index = 0; index < text.length; index += 1) {
			if (text[index] !== query[letter]) continue;
			for (let before = 0; before < index; before += 1) {
				if (!Number.isFinite(scores[before])) continue;
				const gap = index - before - 1;
				const score = scores[before] + (gap === 0 ? 3 : Math.max(1, 3 - gap));
				if (score > nextScores[index] || (score === nextScores[index] && starts[before] < nextStarts[index])) {
					nextScores[index] = score;
					nextStarts[index] = starts[before];
				}
			}
		}
		scores = nextScores;
		starts = nextStarts;
	}
	let best = -Infinity;
	for (let index = 0; index < text.length; index += 1) {
		if (Number.isFinite(scores[index])) best = Math.max(best, scores[index] + Math.max(0, 5 - starts[index]));
	}
	return Number.isFinite(best) ? best : null;
}

// Exact beats a prefix, a prefix a word's prefix, then a substring, then the
// letters in order; a weaker kind never outranks a stronger one.
function matchText(query, text, fuzzy) {
	if (text === query) return { tier: 5, score: 100 };
	if (text.startsWith(query)) return { tier: 4, score: (query.length / text.length) * 100 };
	for (let from = text.indexOf(query); from !== -1; from = text.indexOf(query, from + 1)) {
		if (from === 0 || !isWordCharacter(text[from - 1])) return { tier: 3, score: 100 - from };
	}
	const at = text.indexOf(query);
	if (at !== -1) return { tier: 2, score: 100 - at };
	if (!fuzzy) return null;
	const score = subsequenceScore(query, text);
	return score === null ? null : { tier: 1, score };
}

function better(candidate, current) {
	if (!current) return true;
	if (candidate.tier !== current.tier) return candidate.tier > current.tier;
	if (candidate.shown !== current.shown) return candidate.shown;
	return candidate.score > current.score;
}

// The countries a query finds, best first: each by its shown name, its code,
// its other names and its M49 name, whichever matches best. An empty query
// finds them all, in their order.
export function findCountries(query) {
	const wanted = normalize(query);
	if (!wanted) return [...COUNTRIES];
	const rank = (fuzzy) => {
		const found = [];
		for (const { country, texts } of SEARCH_INDEX) {
			let best = null;
			for (const { text, shown } of texts) {
				const match = matchText(wanted, text, fuzzy);
				if (match && better({ ...match, shown }, best)) best = { ...match, shown };
			}
			if (best) found.push({ country, best });
		}
		return found;
	};
	// Typo scoring is needed only if no ordinary match exists. One or two
	// letters never use it, so normal typing takes the inexpensive path.
	let found = rank(false);
	if (!found.length && wanted.length > 2) found = rank(true);
	found.sort((a, b) =>
		b.best.tier - a.best.tier
		|| Number(b.best.shown) - Number(a.best.shown)
		|| b.best.score - a.best.score
		|| a.country.label.localeCompare(b.country.label, "en"));
	// One or two letters are the start of a name or a code ("us" is not inside
	// "Australia"); letters merely in order are a guess for a typo, offered only
	// when nothing matches by name, code or other name.
	const lowest = wanted.length <= 2 ? 3 : found.length > 0 && found[0].best.tier > 1 ? 2 : 1;
	return found.filter(({ best }) => best.tier >= lowest).map(({ country }) => country);
}

// The country a text names exactly, by its shown name, code, other name or
// M49 name, or null.
export function countryNamed(text) {
	const wanted = normalize(text);
	if (!wanted) return null;
	return EXACT_NAMES.get(wanted) ?? null;
}
