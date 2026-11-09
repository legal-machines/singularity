// The countries and territories an organisation can be registered in, for the
// onboarding's Country field: every UN M49 "Country or Area", and Taiwan and
// Kosovo, where organisations are incorporated too though M49 lists neither.
// Each has its ISO 3166 code, so "US" finds the United States, and the short
// English name the field shows and the registration sends; the M49 name and a
// few common other names are searched too. The list and its ranking come from
// the Alpha onboarding's country combobox (git history:
// code/frontend/src/entities/country, before 25b02afc7).

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
	US: ["America", "United States of America", "Estados Unidos", "États-Unis", "Vereinigte Staaten", "\u0421\u0428\u0410", "\u0421\u043e\u0435\u0434\u0438\u043d\u0451\u043d\u043d\u044b\u0435 \u0428\u0442\u0430\u0442\u044b", "\u0410\u043c\u0435\u0440\u0438\u043a\u0430"],
	GB: ["UK", "Britain", "Great Britain", "England", "Scotland", "Wales", "Northern Ireland", "Royaume-Uni", "Vereinigtes Königreich", "Reino Unido", "\u0412\u0435\u043b\u0438\u043a\u043e\u0431\u0440\u0438\u0442\u0430\u043d\u0438\u044f", "\u0410\u043d\u0433\u043b\u0438\u044f"],
	AE: ["UAE", "Emirates", "Dubai", "Abu Dhabi", "الإمارات", "\u041e\u0410\u042d"],
	SA: ["KSA", "Saudi", "السعودية"],
	ZA: ["RSA", "Suid-Afrika", "\u042e\u0410\u0420"],
	KR: ["Korea", "ROK", "대한민국", "한국", "\u042e\u0436\u043d\u0430\u044f \u041a\u043e\u0440\u0435\u044f"],
	KP: ["DPRK", "조선", "\u0421\u0435\u0432\u0435\u0440\u043d\u0430\u044f \u041a\u043e\u0440\u0435\u044f"],
	CN: ["PRC", "People's Republic of China", "Mainland China", "中国", "中华人民共和国", "\u041a\u0438\u0442\u0430\u0439"],
	TW: ["ROC", "Republic of China", "Chinese Taipei", "台灣", "台湾", "\u0422\u0430\u0439\u0432\u0430\u043d\u044c"],
	HK: ["Hong Kong SAR", "香港", "\u0413\u043e\u043d\u043a\u043e\u043d\u0433"],
	MO: ["Macao", "Macao SAR", "澳門", "澳门"],
	JP: ["Nippon", "Nihon", "日本", "\u042f\u043f\u043e\u043d\u0438\u044f"],
	DE: ["Deutschland", "Allemagne", "Alemania", "Germania", "\u0413\u0435\u0440\u043c\u0430\u043d\u0438\u044f"],
	FR: ["Frankreich", "Francia", "\u0424\u0440\u0430\u043d\u0446\u0438\u044f"],
	ES: ["España", "Spanien", "Espagne", "\u0418\u0441\u043f\u0430\u043d\u0438\u044f"],
	IT: ["Italia", "Italien", "\u0418\u0442\u0430\u043b\u0438\u044f"],
	PT: ["\u041f\u043e\u0440\u0442\u0443\u0433\u0430\u043b\u0438\u044f"],
	NL: ["Holland", "Nederland", "Niederlande", "Pays-Bas", "Países Bajos", "\u041d\u0438\u0434\u0435\u0440\u043b\u0430\u043d\u0434\u044b", "\u0413\u043e\u043b\u043b\u0430\u043d\u0434\u0438\u044f"],
	BE: ["België", "Belgique", "Belgien", "\u0411\u0435\u043b\u044c\u0433\u0438\u044f"],
	CH: ["Swiss", "Schweiz", "Suisse", "Svizzera", "Svizra", "\u0428\u0432\u0435\u0439\u0446\u0430\u0440\u0438\u044f"],
	AT: ["Österreich", "Autriche", "\u0410\u0432\u0441\u0442\u0440\u0438\u044f"],
	LU: ["Luxemburg", "Lëtzebuerg", "\u041b\u044e\u043a\u0441\u0435\u043c\u0431\u0443\u0440\u0433"],
	LI: ["\u041b\u0438\u0445\u0442\u0435\u043d\u0448\u0442\u0435\u0439\u043d"],
	IE: ["Éire", "Republic of Ireland", "\u0418\u0440\u043b\u0430\u043d\u0434\u0438\u044f"],
	SE: ["Sverige", "\u0428\u0432\u0435\u0446\u0438\u044f"],
	NO: ["Norge", "Noreg", "\u041d\u043e\u0440\u0432\u0435\u0433\u0438\u044f"],
	DK: ["Danmark", "\u0414\u0430\u043d\u0438\u044f"],
	FI: ["Suomi", "\u0424\u0438\u043d\u043b\u044f\u043d\u0434\u0438\u044f"],
	IS: ["Ísland", "\u0418\u0441\u043b\u0430\u043d\u0434\u0438\u044f"],
	FO: ["Faroes", "Føroyar"],
	GL: ["Kalaallit Nunaat", "Grønland"],
	PL: ["Polska", "\u041f\u043e\u043b\u044c\u0448\u0430"],
	CZ: ["Czech Republic", "Česko", "Česká republika", "\u0427\u0435\u0445\u0438\u044f"],
	SK: ["Slovensko", "\u0421\u043b\u043e\u0432\u0430\u043a\u0438\u044f"],
	HU: ["Magyarország", "\u0412\u0435\u043d\u0433\u0440\u0438\u044f"],
	RO: ["România", "\u0420\u0443\u043c\u044b\u043d\u0438\u044f"],
	BG: ["\u0411\u044a\u043b\u0433\u0430\u0440\u0438\u044f", "\u0411\u043e\u043b\u0433\u0430\u0440\u0438\u044f"],
	GR: ["Hellas", "Ελλάδα", "Ελλάς", "\u0413\u0440\u0435\u0446\u0438\u044f"],
	CY: ["Κύπρος", "Kıbrıs", "\u041a\u0438\u043f\u0440"],
	MT: ["\u041c\u0430\u043b\u044c\u0442\u0430"],
	HR: ["Hrvatska", "\u0425\u043e\u0440\u0432\u0430\u0442\u0438\u044f"],
	SI: ["Slovenija", "\u0421\u043b\u043e\u0432\u0435\u043d\u0438\u044f"],
	RS: ["Srbija", "\u0421\u0440\u0431\u0438\u0458\u0430", "\u0421\u0435\u0440\u0431\u0438\u044f"],
	BA: ["Bosnia", "Herzegovina", "Bosna i Hercegovina", "\u0411\u043e\u0441\u043d\u0438\u044f \u0438 \u0413\u0435\u0440\u0446\u0435\u0433\u043e\u0432\u0438\u043d\u0430"],
	ME: ["Crna Gora", "\u0426\u0440\u043d\u0430 \u0413\u043e\u0440\u0430", "\u0427\u0435\u0440\u043d\u043e\u0433\u043e\u0440\u0438\u044f"],
	MK: ["Macedonia", "FYROM", "\u0421\u0435\u0432\u0435\u0440\u043d\u0430 \u041c\u0430\u043a\u0435\u0434\u043e\u043d\u0438\u0458\u0430", "\u0421\u0435\u0432\u0435\u0440\u043d\u0430\u044f \u041c\u0430\u043a\u0435\u0434\u043e\u043d\u0438\u044f"],
	AL: ["Shqipëri", "Shqipëria", "\u0410\u043b\u0431\u0430\u043d\u0438\u044f"],
	XK: ["Kosova", "\u041a\u043e\u0441\u043e\u0432\u043e"],
	UA: ["Ukraina", "\u0423\u043a\u0440\u0430\u0457\u043d\u0430", "\u0423\u043a\u0440\u0430\u0438\u043d\u0430"],
	BY: ["Belorussia", "Byelorussia", "\u0411\u0435\u043b\u0430\u0440\u0443\u0441\u044c", "\u0411\u0435\u043b\u043e\u0440\u0443\u0441\u0441\u0438\u044f"],
	RU: ["Rossiya", "\u0420\u043e\u0441\u0441\u0438\u044f", "\u0420\u043e\u0441\u0441\u0438\u0439\u0441\u043a\u0430\u044f \u0424\u0435\u0434\u0435\u0440\u0430\u0446\u0438\u044f", "\u0420\u0424"],
	MD: ["\u041c\u043e\u043b\u0434\u043e\u0432\u0430", "\u041c\u043e\u043b\u0434\u0430\u0432\u0438\u044f"],
	LT: ["Lietuva", "\u041b\u0438\u0442\u0432\u0430"],
	LV: ["Latvija", "\u041b\u0430\u0442\u0432\u0438\u044f"],
	EE: ["Eesti", "\u042d\u0441\u0442\u043e\u043d\u0438\u044f"],
	KZ: ["Qazaqstan", "\u049a\u0430\u0437\u0430\u049b\u0441\u0442\u0430\u043d", "\u041a\u0430\u0437\u0430\u0445\u0441\u0442\u0430\u043d"],
	UZ: ["Oʻzbekiston", "\u0423\u0437\u0431\u0435\u043a\u0438\u0441\u0442\u0430\u043d"],
	KG: ["Kyrgyz Republic", "Kirghizia", "\u041a\u044b\u0440\u0433\u044b\u0437\u0441\u0442\u0430\u043d", "\u041a\u0438\u0440\u0433\u0438\u0437\u0438\u044f"],
	TJ: ["\u0422\u043e\u04b7\u0438\u043a\u0438\u0441\u0442\u043e\u043d", "\u0422\u0430\u0434\u0436\u0438\u043a\u0438\u0441\u0442\u0430\u043d"],
	TM: ["Türkmenistan", "\u0422\u0443\u0440\u043a\u043c\u0435\u043d\u0438\u0441\u0442\u0430\u043d"],
	AZ: ["Azərbaycan", "\u0410\u0437\u0435\u0440\u0431\u0430\u0439\u0434\u0436\u0430\u043d"],
	AM: ["Hayastan", "Հայաստան", "\u0410\u0440\u043c\u0435\u043d\u0438\u044f"],
	GE: ["Sakartvelo", "საქართველო", "\u0413\u0440\u0443\u0437\u0438\u044f"],
	TR: ["Turkey", "\u0422\u0443\u0440\u0446\u0438\u044f"],
	IL: ["ישראל", "\u0418\u0437\u0440\u0430\u0438\u043b\u044c"],
	PS: ["فلسطين"],
	JO: ["الأردن"],
	LB: ["لبنان"],
	SY: ["سوريا"],
	IQ: ["العراق"],
	IR: ["Persia", "ایران", "\u0418\u0440\u0430\u043d"],
	KW: ["الكويت"],
	BH: ["البحرين"],
	QA: ["قطر"],
	OM: ["عُمان", "عمان"],
	YE: ["اليمن"],
	EG: ["Misr", "مصر", "\u0415\u0433\u0438\u043f\u0435\u0442"],
	MA: ["Maroc", "المغرب"],
	DZ: ["Algérie", "الجزائر"],
	TN: ["Tunisie", "تونس"],
	LY: ["ليبيا"],
	IN: ["Bharat", "Hindustan", "भारत", "\u0418\u043d\u0434\u0438\u044f"],
	PK: ["پاکستان"],
	BD: ["বাংলাদেশ"],
	LK: ["Ceylon", "ශ්‍රී ලංකාව", "இலங்கை"],
	NP: ["नेपाल"],
	MM: ["Burma", "မြန်မာ"],
	TH: ["Siam", "ประเทศไทย", "\u0422\u0430\u0438\u043b\u0430\u043d\u0434"],
	VN: ["\u0412\u044c\u0435\u0442\u043d\u0430\u043c"],
	LA: ["ລາວ"],
	KH: ["Kampuchea", "កម្ពុជា"],
	SG: ["Singapura", "新加坡", "\u0421\u0438\u043d\u0433\u0430\u043f\u0443\u0440"],
	PH: ["Pilipinas", "Filipinas"],
	TL: ["East Timor"],
	MN: ["\u041c\u043e\u043d\u0433\u043e\u043b \u0423\u043b\u0441", "\u041c\u043e\u043d\u0433\u043e\u043b\u0438\u044f"],
	MX: ["\u041c\u0435\u043a\u0441\u0438\u043a\u0430"],
	BR: ["Brasil", "\u0411\u0440\u0430\u0437\u0438\u043b\u0438\u044f"],
	AR: ["\u0410\u0440\u0433\u0435\u043d\u0442\u0438\u043d\u0430"],
	CL: ["\u0427\u0438\u043b\u0438"],
	DO: ["República Dominicana", "Dominican"],
	CA: ["Kanada", "\u041a\u0430\u043d\u0430\u0434\u0430"],
	NZ: ["Aotearoa", "\u041d\u043e\u0432\u0430\u044f \u0417\u0435\u043b\u0430\u043d\u0434\u0438\u044f"],
	AU: ["\u0410\u0432\u0441\u0442\u0440\u0430\u043b\u0438\u044f"],
	CI: ["Ivory Coast"],
	CD: ["DRC", "DR Congo", "Congo-Kinshasa", "Zaire"],
	CG: ["Congo-Brazzaville"],
	CF: ["CAR"],
	CV: ["Cape Verde"],
	SZ: ["Swaziland"],
	GW: ["Guinea Bissau"],
	PG: ["PNG"],
	VA: ["Vatican", "Vatican City", "\u0412\u0430\u0442\u0438\u043a\u0430\u043d"],
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
