// The calendar registry: DATA, not code. Each entry is one published calendar
// of closing days, with its source, the years it is known for, whether a
// closure can be declared at short notice, and the authorities and places it
// is the calendar of. BusinessCalendar.cpp reads it generically - no calendar
// has a code path of its own. Included once, inside registry()'s initializer.
//
// Fields: id, name, source, serves {authority, place}, first_year, last_year,
// unscheduled_closures, includes, closed (rules), closed_dates, not_closed_dates,
// working_dates, possible (rules), possible_dates.
//
// A rule: fixed(name, month, day, observance), nth(name, month, weekday, n)
// (weekday Monday 0 ... Sunday 6; n 0 is the last), on_or_after(name, month,
// day, weekday), easter(name, offset); from/to years where a holiday began or
// ended.

// US federal legal public holidays: 5 U.S.C. 6103(a); a Saturday holiday is
// observed on the Friday, a Sunday one on the Monday (5 U.S.C. 6103(b);
// Executive Order 11582). Possible: days an executive order closed federal
// offices - not legal public holidays, but a looser "federal holiday" may
// reach them.
{
	"us_federal", "US federal holidays", "5 U.S.C. 6103; Executive Order 11582",
	{{Authority::PublicHolidays, "united states"}},
	1986, 2100, true, {},
	{
		fixed("New Year's Day", 1, 1, Observance::SaturdayToFridaySundayToMonday),
		nth("Birthday of Martin Luther King, Jr.", 1, 0, 3),
		nth("Washington's Birthday", 2, 0, 3),
		nth("Memorial Day", 5, 0, 0),
		fixed("Juneteenth National Independence Day", 6, 19, Observance::SaturdayToFridaySundayToMonday, 2021),
		fixed("Independence Day", 7, 4, Observance::SaturdayToFridaySundayToMonday),
		nth("Labor Day", 9, 0, 1),
		nth("Columbus Day", 10, 0, 2),
		fixed("Veterans Day", 11, 11, Observance::SaturdayToFridaySundayToMonday),
		nth("Thanksgiving Day", 11, 3, 4),
		fixed("Christmas Day", 12, 25, Observance::SaturdayToFridaySundayToMonday),
	},
	{}, {}, {},
	{},
	{{2012, 12, 24}, {2014, 12, 26}, {2015, 12, 24}, {2018, 12, 5}, {2018, 12, 24}, {2019, 12, 24}, {2020, 12, 24},
	 {2024, 12, 24}, {2025, 1, 9}, {2025, 12, 24}, {2025, 12, 26}},
},

// Days the federal government's offices are closed: the legal public holidays
// and the days executive orders closed them.
{
	"us_federal_closures", "US federal government closures", "5 U.S.C. 6103; executive orders closing executive departments",
	{{Authority::GovernmentOffices, "united states"}},
	1986, 2100, true, {"us_federal"},
	{},
	{{2012, 12, 24}, {2014, 12, 26}, {2015, 12, 24}, {2018, 12, 5}, {2018, 12, 24}, {2019, 12, 24}, {2020, 12, 24},
	 {2024, 12, 24}, {2025, 1, 9}, {2025, 12, 24}, {2025, 12, 26}},
	{}, {},
	{}, {},
},

// The Federal Reserve Banks' holiday schedule: the federal holidays, a Sunday
// one observed on the Monday, a Saturday one NOT observed on the Friday.
{
	"us_federal_reserve", "Federal Reserve holidays", "Federal Reserve Bank Services holiday schedule",
	{{Authority::CentralBank, "united states"}},
	1986, 2100, true, {},
	{
		fixed("New Year's Day", 1, 1, Observance::SundayToMonday),
		nth("Birthday of Martin Luther King, Jr.", 1, 0, 3),
		nth("Washington's Birthday", 2, 0, 3),
		nth("Memorial Day", 5, 0, 0),
		fixed("Juneteenth National Independence Day", 6, 19, Observance::SundayToMonday, 2021),
		fixed("Independence Day", 7, 4, Observance::SundayToMonday),
		nth("Labor Day", 9, 0, 1),
		nth("Columbus Day", 10, 0, 2),
		fixed("Veterans Day", 11, 11, Observance::SundayToMonday),
		nth("Thanksgiving Day", 11, 3, 4),
		fixed("Christmas Day", 12, 25, Observance::SundayToMonday),
	},
	{}, {}, {},
	{}, {},
},

// Commercial banks in New York: closed on the Federal Reserve's holidays.
// Possible: the other New York public holidays banks are authorized to close
// on (N.Y. Gen. Constr. Law 24: Lincoln's Birthday, Election Day), and the
// days the Superintendent authorized emergency closures.
{
	"us_ny_banks", "New York bank holidays", "Federal Reserve holiday schedule; N.Y. Gen. Constr. Law 24; N.Y. Banking Law",
	{{Authority::Banks, "new york"}},
	1986, 2100, true, {"us_federal_reserve"},
	{},
	{}, {}, {},
	{
		fixed("Lincoln's Birthday", 2, 12, Observance::SundayToMonday),
		on_or_after("Election Day", 11, 2, 1),
	},
	{{2012, 10, 29}, {2012, 10, 30}},
},

// New York public holidays (N.Y. Gen. Constr. Law 24): a Sunday holiday is
// followed by a Monday one; Flag Day, always a Sunday, is not moved.
{
	"us_ny_public_holidays", "New York public holidays", "N.Y. Gen. Constr. Law 24",
	{{Authority::PublicHolidays, "new york"}},
	2021, 2100, true, {},
	{
		fixed("New Year's Day", 1, 1, Observance::SundayToMonday),
		nth("Dr. Martin Luther King, Jr. Day", 1, 0, 3),
		fixed("Lincoln's Birthday", 2, 12, Observance::SundayToMonday),
		nth("Washington's Birthday", 2, 0, 3),
		nth("Memorial Day", 5, 0, 0),
		fixed("Juneteenth", 6, 19, Observance::SundayToMonday),
		fixed("Independence Day", 7, 4, Observance::SundayToMonday),
		nth("Labor Day", 9, 0, 1),
		nth("Columbus Day", 10, 0, 2),
		on_or_after("Election Day", 11, 2, 1),
		fixed("Veterans' Day", 11, 11, Observance::SundayToMonday),
		nth("Thanksgiving Day", 11, 3, 4),
		fixed("Christmas Day", 12, 25, Observance::SundayToMonday),
	},
	{}, {}, {},
	{}, {},
},

// The New York Stock Exchange's trading holidays (NYSE Rule 7.2): a Saturday
// holiday is observed on the Friday, except New Year's Day, whose Friday closes
// a year; closures for national days of mourning and emergencies, dated.
{
	"us_nyse", "New York Stock Exchange holidays", "NYSE Rule 7.2 and published holiday schedules",
	{{Authority::Exchange, "nyse"}},
	2022, 2028, true, {},
	{
		fixed("New Year's Day", 1, 1, Observance::SundayToMonday),
		nth("Martin Luther King, Jr. Day", 1, 0, 3),
		nth("Washington's Birthday", 2, 0, 3),
		easter("Good Friday", -2),
		nth("Memorial Day", 5, 0, 0),
		fixed("Juneteenth National Independence Day", 6, 19, Observance::SaturdayToFridaySundayToMonday),
		fixed("Independence Day", 7, 4, Observance::SaturdayToFridaySundayToMonday),
		nth("Labor Day", 9, 0, 1),
		nth("Thanksgiving Day", 11, 3, 4),
		fixed("Christmas Day", 12, 25, Observance::SaturdayToFridaySundayToMonday),
	},
	{{2025, 1, 9}},
	{}, {},
	{}, {},
},

// Bank holidays in England and Wales (Banking and Financial Dealings Act 1971,
// Schedule 1, and royal proclamations): a weekend holiday is substituted by the
// next weekday that is not already one. Proclaimed moves and extra days, dated.
{
	"gb_england_wales", "England and Wales bank holidays", "Banking and Financial Dealings Act 1971; royal proclamations (GOV.UK)",
	{{Authority::Banks, "london"}, {Authority::Banks, "england"}, {Authority::PublicHolidays, "england"},
	 {Authority::PublicHolidays, "london"}},
	2018, 2027, true, {},
	{
		fixed("New Year's Day", 1, 1, Observance::SubstituteNextWeekday),
		easter("Good Friday", -2),
		easter("Easter Monday", 1),
		nth("Early May bank holiday", 5, 0, 1),
		nth("Spring bank holiday", 5, 0, 0),
		nth("Summer bank holiday", 8, 0, 0),
		fixed("Christmas Day", 12, 25, Observance::SubstituteNextWeekday),
		fixed("Boxing Day", 12, 26, Observance::SubstituteNextWeekday),
	},
	{{2020, 5, 8}, {2022, 6, 2}, {2022, 6, 3}, {2022, 9, 19}, {2023, 5, 8}},
	{{2020, 5, 4}, {2022, 5, 30}},
	{},
	{}, {},
},

// TARGET2 / T2 closing days (ECB): New Year's Day, Good Friday, Easter Monday,
// 1 May, Christmas Day and 26 December; never moved.
{
	"target2", "TARGET closing days", "European Central Bank, TARGET closing days (since 2002)",
	{{Authority::PaymentSystem, "target2"}},
	2002, 2100, false, {},
	{
		fixed("New Year's Day", 1, 1, Observance::None),
		easter("Good Friday", -2),
		easter("Easter Monday", 1),
		fixed("Labour Day", 5, 1, Observance::None),
		fixed("Christmas Day", 12, 25, Observance::None),
		fixed("26 December", 12, 26, Observance::None),
	},
	{}, {}, {},
	{}, {},
},

// The Russian production calendar: the non-working holidays of Labour Code
// art. 112, and each published year's transfers - the weekdays a government
// decree made non-working and the Saturdays it made working. Decrees No. 1505
// (2023), No. 1306 (2024), No. 1335 (2025) and the 2026 production calendar.
// Non-working days a presidential decree may declare at short notice are
// unscheduled.
{
	"ru_production", "Russian production calendar", "Labour Code art. 112; Government decrees on transfers of days off",
	{{Authority::PublicHolidays, "russia"}, {Authority::Banks, "russia"}, {Authority::Banks, "moscow"},
	 {Authority::GovernmentOffices, "russia"}, {Authority::CentralBank, "russia"}},
	2023, 2026, true, {},
	{
		fixed("New Year holidays", 1, 1, Observance::None), fixed("New Year holidays", 1, 2, Observance::None),
		fixed("New Year holidays", 1, 3, Observance::None), fixed("New Year holidays", 1, 4, Observance::None),
		fixed("New Year holidays", 1, 5, Observance::None), fixed("New Year holidays", 1, 6, Observance::None),
		fixed("Christmas", 1, 7, Observance::None), fixed("New Year holidays", 1, 8, Observance::None),
		fixed("Defender of the Fatherland Day", 2, 23, Observance::None),
		fixed("International Women's Day", 3, 8, Observance::None),
		fixed("Spring and Labour Day", 5, 1, Observance::None),
		fixed("Victory Day", 5, 9, Observance::None),
		fixed("Russia Day", 6, 12, Observance::None),
		fixed("Unity Day", 11, 4, Observance::None),
	},
	{
		{2023, 2, 24}, {2023, 5, 8}, {2023, 11, 6},
		{2024, 4, 29}, {2024, 4, 30}, {2024, 5, 10}, {2024, 12, 30}, {2024, 12, 31},
		{2025, 5, 2}, {2025, 5, 8}, {2025, 6, 13}, {2025, 11, 3}, {2025, 12, 31},
		{2026, 1, 9}, {2026, 3, 9}, {2026, 5, 11}, {2026, 12, 31},
	},
	{},
	{{2024, 4, 27}, {2024, 11, 2}, {2024, 12, 28}, {2025, 11, 1}},
	{}, {},
},
