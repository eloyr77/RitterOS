// Intl (27s): English formatting, plural rules, sorting, relative times,
// lists and locale tags. Dates name their time zone: the guest's and the
// Mac's local zones differ.
function nf(o, xs) {
    var f = new Intl.NumberFormat("en-US", o);
    print(JSON.stringify(o), xs.map(function (x) { return f.format(x); }).join(" | "));
}
nf({}, [0, -0, 1234.5678, -1234.5678, 0.000123456, 1e21, 123456789.123, NaN, Infinity, -Infinity, 2.5, 999.9999, 1.005, 2 ** 70]);
nf({ style: "percent" }, [0, 0.256, -0.5, 1.23456, 12, 0.07]);
nf({ style: "currency", currency: "USD" }, [0, 1234.5, -1234.567, 0.005, 1e9]);
nf({ style: "currency", currency: "EUR" }, [1234.5]);
nf({ style: "currency", currency: "JPY" }, [1234.5]);
nf({ style: "currency", currency: "GBP", currencyDisplay: "code" }, [1234.5]);
nf({ style: "currency", currency: "USD", currencyDisplay: "name" }, [1, 1234.5]);
nf({ style: "currency", currency: "USD", currencyDisplay: "name", maximumFractionDigits: 0 }, [1, 2]);
nf({ style: "currency", currency: "USD", currencySign: "accounting" }, [-5, 5]);
nf({ style: "currency", currency: "CAD" }, [1]);
nf({ style: "currency", currency: "CAD", currencyDisplay: "narrowSymbol" }, [1]);
nf({ style: "currency", currency: "CHF" }, [1]);
nf({ style: "currency", currency: "XYZ" }, [1]);
nf({ notation: "compact" }, [0, 999, 1000, 1234, 12345, 123456, 1234567, 1.5e9, 2.5e12, 1e15, -45678, 0.12, 999999, 999.9]);
nf({ notation: "compact", compactDisplay: "long" }, [1234, 1234567, 1.5e9, 2.5e12, 1000]);
nf({ notation: "compact", style: "currency", currency: "USD" }, [1234567]);
nf({ notation: "scientific" }, [0, 1234.5, 0.00012, -5, 1e21, 9.9999]);
nf({ notation: "engineering" }, [1234.5, 0.00012, 123456, 999999]);
nf({ minimumFractionDigits: 2 }, [1, 1.005, 1.23456]);
nf({ maximumFractionDigits: 0 }, [1.5, 2.5, -1.5]);
nf({ minimumIntegerDigits: 3 }, [5, 1234]);
nf({ maximumSignificantDigits: 3 }, [123456, 0.00123456, 1.995]);
nf({ minimumSignificantDigits: 5 }, [1, 1.5]);
nf({ useGrouping: false }, [1234567.5]);
nf({ useGrouping: "min2" }, [1234, 12345]);
nf({ signDisplay: "always" }, [1, 0, -0, -1, NaN]);
nf({ signDisplay: "exceptZero" }, [1, 0, -0, -1]);
nf({ signDisplay: "negative" }, [-0, -1, 0, -0.0001]);
nf({ roundingMode: "floor", maximumFractionDigits: 0 }, [1.7, -1.2]);
nf({ roundingMode: "halfEven", maximumFractionDigits: 0 }, [2.5, 3.5]);
nf({ roundingIncrement: 5, maximumFractionDigits: 2, minimumFractionDigits: 2 }, [1.234]);
nf({ roundingPriority: "lessPrecision", maximumSignificantDigits: 2, maximumFractionDigits: 2 }, [1.234]);
nf({ maximumFractionDigits: 2, minimumFractionDigits: 2, trailingZeroDisplay: "stripIfInteger" }, [1, 1.5, 1.001]);
nf({ style: "unit", unit: "kilometer-per-hour" }, [50]);
nf({ style: "unit", unit: "liter", unitDisplay: "long" }, [1, 16]);
nf({ style: "unit", unit: "megabyte" }, [1.5]);
nf({ style: "unit", unit: "second", unitDisplay: "narrow" }, [3]);
nf({ style: "unit", unit: "byte", notation: "compact", unitDisplay: "narrow" }, [123456]);
nf({ style: "unit", unit: "celsius" }, [20]);
nf({ style: "unit", unit: "fahrenheit", unitDisplay: "long" }, [1, 2]);
nf({ style: "unit", unit: "foot" }, [12]);
nf({ style: "unit", unit: "megabyte-per-second" }, [5]);
var parts = function (f, x) { return JSON.stringify(f.formatToParts(x)); };
print(parts(new Intl.NumberFormat("en-US", { style: "currency", currency: "usd" }), -1234.5));
print(parts(new Intl.NumberFormat("en-US", { notation: "compact" }), 1500));
print(parts(new Intl.NumberFormat("en-US", { notation: "scientific" }), -1500));
print(JSON.stringify(new Intl.NumberFormat("en-US", { style: "currency", currency: "usd" }).resolvedOptions()));
print(JSON.stringify(new Intl.NumberFormat("en", { notation: "compact" }).resolvedOptions()));
print(new Intl.NumberFormat("en-US").formatRange(3, 5), new Intl.NumberFormat("en-US", { style: "currency", currency: "USD" }).formatRange(3, 5));
print((1234.5).toLocaleString(), (1234.5).toLocaleString("en-US", { style: "currency", currency: "USD" }), [1234.5, "x", null].toLocaleString());
["currency", "maximumFractionDigits", "style"].forEach(function (k) {
    var o = { style: "currency" };
    if (k === "maximumFractionDigits") o = { maximumFractionDigits: 101 };
    if (k === "style") o = { style: "bogus" };
    try { new Intl.NumberFormat("en-US", o); } catch (e) { print(e.name + ": " + e.message); }
});

var d = new Date(Date.UTC(2026, 9, 26, 15, 4, 5, 678));
function df(o, t) {
    var f = new Intl.DateTimeFormat("en-US", Object.assign({ timeZone: "UTC" }, o));
    print(JSON.stringify(o), "=>", f.format(t === undefined ? d : t));
}
["full", "long", "medium", "short"].forEach(function (s) {
    df({ dateStyle: s });
    df({ timeStyle: s });
    df({ dateStyle: s, timeStyle: s === "full" ? "short" : "medium" });
});
df({});
df({ year: "numeric", month: "long", day: "numeric" });
df({ month: "short", day: "numeric" });
df({ weekday: "long" });
df({ year: "numeric", month: "2-digit", day: "2-digit" });
df({ month: "long", year: "numeric" });
df({ hour: "numeric", minute: "2-digit" });
df({ weekday: "short", month: "short", day: "numeric" });
df({ year: "numeric", month: "long", day: "numeric", hour: "numeric", minute: "2-digit" });
df({ year: "numeric", month: "numeric", day: "numeric", hour: "numeric", minute: "2-digit" });
df({ weekday: "long", year: "numeric", month: "long", day: "numeric", hour: "numeric", minute: "2-digit", second: "2-digit" });
df({ hour: "2-digit", minute: "2-digit", hour12: false });
df({ hour: "numeric", minute: "numeric", second: "numeric", fractionalSecondDigits: 2 });
df({ hour: "numeric", minute: "numeric", timeZoneName: "short" });
df({ timeZoneName: "long" });
df({ timeZoneName: "shortOffset" });
df({ year: "2-digit" });
df({ month: "narrow" });
df({ era: "short" });
df({ hourCycle: "h23", hour: "numeric", minute: "numeric" });
df({ hourCycle: "h11", hour: "numeric" }, Date.UTC(2026, 0, 1, 12));
df({ hourCycle: "h24", hour: "numeric" }, Date.UTC(2026, 0, 1, 0));
df({ dayPeriod: "long", hour: "numeric" });
df({ dayPeriod: "short", hour: "numeric" }, Date.UTC(2026, 0, 1, 12));
df({ dayPeriod: "long", hour: "numeric" }, Date.UTC(2026, 0, 1, 22));
df({ month: "short", year: "numeric", hour: "numeric" });
df({ timeZone: "+05:30", timeStyle: "long" });
df({ timeZone: "America/New_York", timeStyle: "full" });
df({ timeZone: "America/New_York", timeStyle: "full" }, Date.UTC(2026, 0, 15, 12));
df({ timeZone: "America/Los_Angeles", dateStyle: "medium", timeStyle: "long" }, Date.UTC(2026, 2, 8, 9, 59));
df({ timeZone: "America/Los_Angeles", dateStyle: "medium", timeStyle: "long" }, Date.UTC(2026, 2, 8, 10, 1));
df({ timeZone: "Europe/Berlin", hour: "numeric", timeZoneName: "long" }, Date.UTC(2026, 2, 29, 0, 59));
df({ timeZone: "Europe/Berlin", hour: "numeric", timeZoneName: "long" }, Date.UTC(2026, 2, 29, 1, 1));
df({ timeZone: "Europe/London", hour: "numeric", timeZoneName: "short" }, Date.UTC(2026, 6, 1));
df({ timeZone: "Asia/Kolkata", hour: "numeric", timeZoneName: "long" });
df({ timeZone: "Australia/Sydney", hour: "numeric", timeZoneName: "long" });
df({ timeZone: "Pacific/Auckland", hour: "numeric", timeZoneName: "shortGeneric" });
df({ timeZone: "Etc/GMT+5", timeStyle: "long" });
print(JSON.stringify(new Intl.DateTimeFormat("en-US", { timeZone: "UTC" }).formatToParts(d)));
print(JSON.stringify(new Intl.DateTimeFormat("en-US", { timeZone: "UTC", dateStyle: "short", timeStyle: "short" }).resolvedOptions()));
print(d.toLocaleString("en-US", { timeZone: "UTC" }), "|", d.toLocaleDateString("en-US", { timeZone: "UTC" }), "|",
      d.toLocaleTimeString("en-US", { timeZone: "UTC" }), "|", d.toLocaleDateString("en-US", { timeZone: "UTC", hour: "numeric" }), "|",
      new Date(NaN).toLocaleString());
print(new Intl.DateTimeFormat("en-US", { timeZone: "UTC", dateStyle: "medium" }).formatRange(new Date(0), new Date(86400000 * 40)));
try { new Intl.DateTimeFormat("en-US", { dateStyle: "medium", weekday: "long" }); } catch (e) { print(e.name + ": " + e.message); }
try { new Intl.DateTimeFormat("en-US", { timeZone: "Mars/Base" }); } catch (e) { print(e.name + ": " + e.message); }
try { new Intl.DateTimeFormat().format(NaN); } catch (e) { print(e.name + ": " + e.message); }

var words = ["résumé", "resume", "Resume", "résume", "RESUME", "a", "A", "á", "b", "B", "ab", "aB", "Ab", "a b", "a-b", "ab1", "ab10", "ab2",
    "Zebra", "apple", "Äpfel", "zoo", "éclair", "eclair", "co-op", "coop", "co op", "10", "9", "1", "x9", "x10", "ß", "ss", "æ", "ae", "ø", "o",
    "ł", "l", "ñ", "n", "Ω", "α", "я", "а", "中", "日", "", " ", "_a", "½", "²"];
print(JSON.stringify(words.slice().sort(new Intl.Collator("en").compare)));
print(JSON.stringify(words.slice().sort(new Intl.Collator("en", { numeric: true }).compare)));
print(JSON.stringify(words.slice().sort(new Intl.Collator("en", { caseFirst: "upper" }).compare)));
print(JSON.stringify(words.slice().sort(function (x, y) { return x.localeCompare(y); })));
["base", "accent", "case", "variant"].forEach(function (s) {
    var c = new Intl.Collator("en", { sensitivity: s }).compare;
    print(s, c("a", "A"), c("a", "á"), c("a", "b"), c("A", "á"), c("ß", "ss"), c("æ", "ae"), c("a", "á"), c("á", "á"));
});
var ip = new Intl.Collator("en", { ignorePunctuation: true }).compare;
print(ip("co-op", "coop"), ip("a b", "ab"), ip("a.b", "ab"));
print(JSON.stringify(new Intl.Collator().resolvedOptions()), "a".localeCompare("B"), "a".localeCompare("A", "en", { sensitivity: "base" }));

var pr = new Intl.PluralRules("en"), po = new Intl.PluralRules("en", { type: "ordinal" });
print([0, 1, 2, 1.5, -1, "1"].map(function (x) { return pr.select(x); }).join(" "), new Intl.PluralRules("en", { minimumFractionDigits: 1 }).select(1));
print([0, 1, 2, 3, 4, 11, 12, 13, 21, 22, 23, 101, 111, 112, 1.5].map(function (x) { return x + ":" + po.select(x); }).join(" "));
print(JSON.stringify(po.resolvedOptions()));

var rt = new Intl.RelativeTimeFormat("en"), ra = new Intl.RelativeTimeFormat("en", { numeric: "auto" });
print(rt.format(-1, "day"), "|", rt.format(1, "day"), "|", rt.format(-3, "hours"), "|", rt.format(2.5, "week"), "|", rt.format(-0, "year"), "|", rt.format(1000, "month"));
print(["year", "quarter", "month", "week", "day", "hour", "minute", "second"].map(function (u) {
    return [-1, 0, 1, 2].map(function (n) { return ra.format(n, u); }).join("/");
}).join(" ; "));
["short", "narrow"].forEach(function (style) {
    var r = new Intl.RelativeTimeFormat("en", { style: style });
    print(["year", "quarter", "month", "week", "day", "hour", "minute", "second"].map(function (u) { return r.format(-2, u) + "/" + r.format(1, u); }).join(" ; "));
});
print(JSON.stringify(rt.formatToParts(-1234.5, "day")), JSON.stringify(ra.formatToParts(-1, "day")));
try { rt.format(1, "fortnight"); } catch (e) { print(e.name + ": " + e.message); }

["conjunction", "disjunction", "unit"].forEach(function (t) {
    ["long", "short", "narrow"].forEach(function (s) {
        var lf = new Intl.ListFormat("en", { type: t, style: s });
        print(t, s, JSON.stringify([[], ["a"], ["a", "b"], ["a", "b", "c"], ["a", "b", "c", "d"]].map(function (x) { return lf.format(x); })));
    });
});
print(JSON.stringify(new Intl.ListFormat("en").formatToParts(["x", "y"])));
try { new Intl.ListFormat().format([1]); } catch (e) { print(e.name + ": " + e.message); }

var lo = new Intl.Locale("en-Latn-us", { region: "GB" });
print(lo.toString(), lo.language, lo.script, lo.region, lo.baseName, lo.maximize().toString(), new Intl.Locale("en").maximize().toString(),
      new Intl.Locale("en-Latn-US").minimize().toString(), new Intl.Locale("zh-Hant-TW").minimize().toString(), lo.calendar, lo.hourCycle);
print(new Intl.Locale("en-US-u-hc-h23-ca-gregory").hourCycle, new Intl.Locale("en", { hourCycle: "h11", numeric: true }).toString());
print(JSON.stringify(Intl.getCanonicalLocales(["EN-us", "en-us", "de-de", "zh-hant-tw", "en-u-ca-gregory-hc-h12", "EN-x-Foo", "iw", "en-u-kn-true"])));
["en_US", "", "en--us"].forEach(function (t) { try { Intl.getCanonicalLocales(t); } catch (e) { print(e.name + ": " + e.message); } });
print(Intl.DateTimeFormat.supportedLocalesOf(["en-GB", "en", "en-US"]).join(), new Intl.NumberFormat("en").resolvedOptions().locale);
print(String(Intl), Object.prototype.toString.call(new Intl.Collator()), typeof Intl.NumberFormat.prototype.formatToParts, Intl.NumberFormat.length,
      Intl.Locale.length, Intl.NumberFormat() instanceof Intl.NumberFormat);
try { Intl.PluralRules(); } catch (e) { print(e.name + ": " + e.message); }
try { Intl.NumberFormat.prototype.format; } catch (e) { print(e.name + ": " + e.message); }
var g = new Intl.NumberFormat().format;
print(g(5), g === new Intl.NumberFormat().format, ["1", "2"].map(new Intl.NumberFormat("en", { minimumFractionDigits: 1 }).format).join());
