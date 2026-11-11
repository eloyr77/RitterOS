// Regular expressions (27o)
function show(m) { return m === null ? "null" : JSON.stringify([...m]) + "@" + m.index + (m.groups ? JSON.stringify(m.groups) : ""); }
var cases = [
    [/abc/, "xxabcxx"], [/a.c/, "abc a\nc"], [/a.c/s, "a\nc"], [/^b/m, "a\nb"], [/^b/, "a\nb"], [/c$/m, "c\nd"],
    [/\d+/, "abc123def456"], [/\w+@\w+\.com/, "mail me at joe@example.com now"], [/\s+/, "a \t\n b"],
    [/(\d{4})-(\d{2})-(\d{2})/, "on 2026-10-26."], [/(?<y>\d{4})-(?<m>\d\d)/, "2026-10"], [/a*/, "bbb"],
    [/a+?/, "aaa"], [/a{2,3}/, "aaaa"], [/a{2,}/, "aaaaa"], [/a{,2}/, "a{,2}"], [/(a|ab)(c|bcd)(d*)/, "abcd"],
    [/\bfoo\b/, "a foo b"], [/\Bfoo/, "afoo"], [/(?=a)a/, "ba"], [/a(?!b)/, "abac"], [/(?<=\$)\d+/, "cost $42"],
    [/(?<!\$)\b\d+/, "$4 5"], [/(a)\1/, "aa"], [/(?<x>.)\k<x>/, "bb"], [/[a-z]+/i, "123ABC"], [/[^aeiou]+/, "aei bcd"],
    [/[\d.]+/, "v1.25b"], [/[\w-]+/, "a-b_c d"], [/A/, "A"], [/\x41/, "A"], [/[à-ÿ]/, "café"],
    [/😀/u, "x😀y"], [/./u, "😀"], [/./, "😀"], [/\p{L}+/u, "héllo1"], [/(?:ab)+/, "ababab"], [/(a)|(b)/, "b"],
    [/(a*)*b/, "aab"], [/(a*)+b/, "b"], [/(?:a|b)*?c/, "abac"], [/^(?:(\d+)\.)?(\d+)$/, "12.34"], [/[]/, "x"],
    [/[^]/, "x"], [/\//, "a/b"], [/[/]/, "a/b"], [/a|/, "b"], [/()/, "x"], [/(?:)/, ""], [/\cJ/, "\n"], [/[\b]/, "\b"],
    [/\0/, "\0"], [/a{1}?/, "aa"], [/x*y+$/, "xxyy"], [/(\w)(\w)?/, "a"], [/(.)(?=(.))/, "ab"], [/\d{3,5}?/, "123456"],
    [/^[A-Z][a-z]*(?: [A-Z][a-z]*)*$/, "Hello World"], [/(\d+)\s*(px|em|%)/, "width: 100 px"], [/[a-c\d]{2}/, "zz1a"],
];
for (var i = 0; i < cases.length; i++) print(String(cases[i][0]), show(cases[i][0].exec(cases[i][1])));
var g = /o/g; print(g.lastIndex, g.test("foo"), g.lastIndex, g.test("foo"), g.lastIndex, g.test("foo"), g.lastIndex);
var y = /a/y; print(y.test("ba"), y.lastIndex, (y.lastIndex = 1, y.test("ba")), y.lastIndex);
print("a1b22c333".match(/\d+/g), "x".match(/y/g), "aaa".match(/a*?/g), "abc".match(/(?:)/g).length);
print([..."a1b2".matchAll(/[a-z](\d)/g)].map(m => m[1] + "@" + m.index).join());
print("2026-10-26".replace(/(\d+)-(\d+)-(\d+)/, "$3/$2/$1"), "aXbXc".replace(/x/gi, "-"), "abc".replace(/(?<l>b)/, "[$<l>]"));
print("abc".replace(/b/, "$$"), "abc".replace(/b/, "$0"), "abc".replace(/(b)/, "$01"), "abc".replace(/b/, "$'$`"));
print("John Smith".replace(/(?<first>\w+) (?<last>\w+)/, (...args) => JSON.stringify(args[args.length - 1])));
print("a-b-c".split(/-/), "a1b2c".split(/\d/), "abc".split(/(?:)/), "a, b,c".split(/\s*,\s*/), "abc".split(/(b)/), "".split(/x/), "test".split(/(?:)/, 2));
print("aaa".lastIndexOf("a"), "abcabc".search(/c/), "ABC".search(/b/i), "x".search(/y/));
var re = /(\d+)/g, s = "1 22 333", m, all = []; while ((m = re.exec(s)) !== null) all.push(m[1] + ":" + re.lastIndex); print(all);
print(/a/gimsuy.flags, /a/.source, new RegExp("a/b").source, String(new RegExp("")), new RegExp("\n").source, /[/]/.source);
print(new RegExp(/abc/g).flags, new RegExp(/abc/g, "i").flags, new RegExp("x", "dg").hasIndices, RegExp.prototype.source, RegExp.prototype.flags);
var d = /(?<word>b+)/d.exec("abbc"); print(JSON.stringify(d.indices), JSON.stringify(d.indices.groups));
try { new RegExp("("); } catch (e) { print(e.name); }
try { new RegExp("a", "gg"); } catch (e) { print(e.name); }
try { new RegExp("[b-a]"); } catch (e) { print(e.name); }
print(/\d/.test("a5"), /^\d+$/.test("123"), /^\d+$/.test("12a"), RegExp("a").test("A"), /A/i.test("a"));
print("CamelCaseString".replace(/([A-Z])/g, " $1").trim(), "snake_case_here".replace(/_(\w)/g, (m, c) => c.toUpperCase()));
print("a.b*c".replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "  multiple   spaces  ".replace(/\s+/g, " "));
print("<b>bold</b> <i>it</i>".replace(/<[^>]+>/g, ""), "x=1&y=2".split("&").map(p => p.split("=")).join(";"));
var email = /^[^\s@]+@[^\s@]+\.[^\s@]+$/; print(email.test("a@b.co"), email.test("a@b"), email.test("a b@c.d"));
var url = /^(https?):\/\/([^/:]+)(?::(\d+))?(\/[^?#]*)?(?:\?([^#]*))?(?:#(.*))?$/;
print(JSON.stringify(url.exec("https://example.com:8080/path/x?q=1#frag")));
print("aBc".replace(/b/i, x => x + x), "test".replace(/t/g, "T"), "xyz".replace(/./g, (c, i) => i));
print(/(?:a|b)+/.exec("abba")[0], /a(?=b)/.exec("ab")[0], /(a)?b/.exec("b")[1], /(?:(a)|b)+/.exec("ab")[1]);
print(/[\s\S]/.test("\n"), /[\w]/.test("é"), /\W/.test("é"), /[^\d]/.test("1"), /\S+/.exec("  ab  ")[0]);
print("ÀÉ".match(/[à-é]+/i), /\u{1F600}/u.test("😀"), /^.$/u.test("😀"), /^.$/.test("😀"));
print(/(a+)+b/.test("aaaaaaaaaaaaaaaaaaaaaaab"), /x(?:a|ab)*y/.test("xababy"), "aaa".replace(/a{2}/, "b"));
print("tel: 555-1234 or 555-9876".match(/\d{3}-\d{4}/g), "The Quick Brown".match(/\b[A-Z]/g).join(""));
print(/\bend\b/.test("the end."), "line1\nline2\nline3".match(/^line\d$/gm), "a\r\nb".split(/\r?\n/));
print("price: $10.99, $5".match(/\$\d+(?:\.\d\d)?/g), "#ff0000".match(/^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i).slice(1));
var lastIdx = /a/g; lastIdx.lastIndex = 5; print(lastIdx.exec("aaa"), lastIdx.lastIndex);
print(RegExp.prototype.toString.call(/a/g), Object.prototype.toString.call(/x/), /x/ instanceof RegExp, typeof /x/);
