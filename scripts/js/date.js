// Date in UTC (27o; the local zone differs between machines)
var d = new Date(Date.UTC(2026, 9, 26, 12, 34, 56, 789));
print(d.getTime(), d.toISOString(), d.toJSON(), d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate(), d.getUTCDay(), d.getUTCHours(), d.getUTCMinutes(), d.getUTCSeconds(), d.getUTCMilliseconds());
print(d.toUTCString(), new Date(0).toUTCString(), new Date(-1).toISOString(), new Date(8.64e15).toISOString(), new Date(-8.64e15).toISOString());
print(new Date(8.64e15 + 1).getTime(), isNaN(new Date("garbage")), String(new Date(NaN)), new Date(NaN).getUTCDate());
var iso = ["2026-10-26", "2026-10-26T12:00:00Z", "2026-10-26T12:00:00.5Z", "2026-10-26T12:00:00+02:00", "2026-10", "2026",
           "+012026-01-01T00:00:00Z", "-000001-01-01T00:00:00Z", "2026-13-01", "2026-02-30", "2026-10-26T25:00Z", "2026-10-26T24:00:00Z"];
for (var i = 0; i < iso.length; i++) print(iso[i], Date.parse(iso[i]));
print(Date.parse("Mon, 26 Oct 2026 12:00:00 GMT"), Date.parse("26 Oct 2026 12:00:00 GMT"), Date.parse("Oct 26, 2026 12:00:00 UTC"), Date.parse("Thu, 01 Jan 1970 00:00:00 GMT+0100"));
print(Date.UTC(2026), Date.UTC(2026, 0), Date.UTC(99, 0), Date.UTC(2026, 12, 1), Date.UTC(2026, 0, 0), Date.UTC(2026, 1, 29), Date.UTC(-1, 0));
var e = new Date(Date.UTC(2020, 1, 29)); e.setUTCFullYear(2021); print(e.toISOString()); e.setUTCMonth(0, 31); print(e.toISOString());
e.setUTCDate(0); print(e.toISOString()); e.setUTCHours(48); print(e.toISOString()); e.setUTCMinutes(-1); print(e.toISOString());
e.setTime(0); print(e.toISOString(), e.setUTCSeconds(30, 500), e.getTime());
print(new Date(2026, 0, 1).getFullYear(), new Date(2026, 0, 1).getMonth(), new Date(2026, 0, 1).getDate(), new Date(2026, 0, 1, 10).getHours());
var l = new Date(2026, 5, 15, 8, 30); print(l.getHours(), l.getMinutes(), l.getDay(), new Date(l.getTime()).getDate());
print(typeof Date.now(), Date.now() > 1.7e12, typeof Date(), new Date(2026, 0) - new Date(2025, 0), +new Date(5));
print(new Date(Date.UTC(1, 0)).getUTCFullYear(), new Date(Date.UTC(1900, 1, 28)).getUTCDay(), new Date(Date.UTC(2000, 1, 29)).getUTCDay());
print(Object.prototype.toString.call(new Date()), new Date(0) == 0, new Date(0) + 0 === new Date(0).toString() + "0", JSON.stringify(new Date(NaN)));
try { new Date(NaN).toISOString(); } catch (er) { print(er.name); }
var days = []; for (var i = 0; i < 7; i++) days.push(new Date(Date.UTC(2026, 9, 25 + i)).getUTCDay()); print(days);
print(new Date(Date.UTC(275760, 8, 13)).getTime(), new Date(Date.UTC(275760, 8, 13, 0, 0, 0, 1)).getTime());
print(new Date("2026-10-26T12:00:00Z").valueOf() === Date.UTC(2026, 9, 26, 12), new Date(d).getTime() === d.getTime());
