// Case mapping by Unicode's tables (27s-2): every code point's upper and
// lower case, the full mappings, the final sigma and case-insensitive
// regular expressions
var words = ["straße", "ǆemal", "ș ț ƒ", "ἀθήνα", "ᾳ ᾼ", "ⴀⴁ Ⴀ", "ⓐⓑ", "ⅰⅱⅲ", "ꙁꙃ", "ᏸᏹ ꭰ", "𐐨𐐩", "𞤢", "ﬁﬂ ﬀ", "ŉ ǰ ΐ", "İstanbul", "ǅ", "Ǳ", "µ", "ÿ", "ſ"];
for (var w of words)
    print(w, w.toUpperCase(), w.toLowerCase(), escape(w.toUpperCase()), escape(w.toLowerCase()));

// a checksum of every code point's mappings
var changed = 0, longer = 0, sum = 0;
for (var c = 0; c <= 0x10FFFF; c++) {
    if (c >= 0xD800 && c <= 0xDFFF)
        continue;
    var s = String.fromCodePoint(c), u = s.toUpperCase(), l = s.toLowerCase();
    if (u === s && l === s)
        continue;
    changed++;
    if (u.length > s.length || l.length > s.length)
        longer++;
    for (var t of [u, l])
        for (var k = 0; k < t.length; k++)
            sum = (sum * 31 + t.charCodeAt(k) + c) % 1000000007;
}
print("changed", changed, "longer", longer, "sum", sum);

// the final sigma: a cased letter before, none after (case-ignorable ones skipped)
for (var w of ["ΣΑΣ", "ΑΣ.", "Σ", "ΑΣ'Α", "Α'Σ", "ΑΣΑ", "ΑΣ́", "ΑΣ Α", "1Σ", "ΑΣ\u{10400}", "ΑΣ\u{1D7CE}", "ᾈΣ"])
    print(escape(w), escape(w.toLowerCase()));

// regular expressions: /iu folds, /i takes the single upper case
print(/ſ/iu.test("S"), /ſ/i.test("S"), /K/iu.test("k"), /K/i.test("k"), /ß/i.test("SS"), /ẞ/iu.test("ß"), /ẞ/i.test("ß"));
print(/[a-z]/iu.test("K"), /[a-z]/i.test("K"), /[^k]/iu.test("K"), /[^k]/i.test("K"), /\w/iu.test("ſ"), /\w/i.test("ſ"));
print(/\W/iu.test("s"), /[\W]/iu.test("S"), /[^\W]/iu.test("ſ"), /\bſ/iu.test("aſ"), /\bſ/iu.test(" ſ"), /ſ\b/i.test("ſ"));
print(/^(.)\1$/iu.test("\u{10400}\u{10428}"), /^(.)\1$/iu.test("ǅǆ"), /^(.)\1$/i.test("ǅǆ"), /(?<=\1(.))x/iu.exec("aAx"));
print(/[\u{10400}-\u{1044F}]+/iu.exec("x\u{10428}\u{10400}y")[0].length, /[ⴀ-ⴥ]+/iu.exec("ႠႡ")[0], /[ⴀ-ⴥ]+/i.exec("ႠႡ")[0], /ǰ/i.test("J̌"));
print(/σ/iu.test("ς"), /Σ/i.test("ς"), /[ς]/i.exec("Σσς"), "ΣΑΣ".replace(/σ/giu, "s"), "ᾳ".replace(/ᾼ/iu, "x"), "ᾳ".replace(/ᾼ/i, "x"));

// every cased character against every other under /iu, as one count
var cands = [];
for (var c = 0; c <= 0x10FFFF; c++) {
    if (c >= 0xD800 && c <= 0xDFFF)
        continue;
    var s = String.fromCodePoint(c);
    if (s.toUpperCase() !== s || s.toLowerCase() !== s)
        cands.push(s);
}
var text = cands.join(" "), hits = 0, classHits = 0;
for (var i = 0; i < cands.length; i += 7) {
    var esc = "\\u{" + cands[i].codePointAt(0).toString(16) + "}";
    hits += text.match(new RegExp(esc, "giu")).length;
    classHits += text.match(new RegExp("[" + esc + "]", "giu")).length;
}
print("iu", cands.length, hits, classHits);
