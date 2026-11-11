// Control flow: loops, labels, switch, exceptions (27o)
var out = [];
for (var i = 0; i < 5; i++) { if (i == 1) continue; if (i == 4) break; out.push(i); } print(out);
var j = 0; while (j < 3) j++; print(j); do { j--; } while (j > 0); print(j);
outer: for (var a = 0; a < 3; a++) { inner: for (var b = 0; b < 3; b++) { if (b == 1) continue outer; if (a == 2) break outer; out.push(a + "" + b); } } print(out);
blk: { out.push("in"); break blk; out.push("never"); } print(out.length);
function sw(x) { switch (x) { case 1: return "one"; case "1": return "string one"; case 2: case 3: return "two or three"; default: return "other"; } }
print(sw(1), sw("1"), sw(3), sw(9), sw());
function fall(x) { var r = ""; switch (x) { default: r += "d"; case 1: r += "1"; break; case 2: r += "2"; } return r; } print(fall(1), fall(2), fall(5));
function tf(x) { try { if (x) throw new Error("e" + x); return "ok"; } catch (e) { return "caught " + e.message; } finally { out.push("f" + x); } }
print(tf(0), tf(1), out.slice(-2));
function nested() { try { try { throw 1; } finally { out.push("inner finally"); } } catch (e) { return "outer caught " + e; } } print(nested());
function finOverride() { try { return 1; } finally { return 2; } } print(finOverride());
function loopFinally() { var r = []; for (var i = 0; i < 3; i++) { try { if (i == 1) continue; r.push(i); } finally { r.push("f" + i); } } return r; } print(loopFinally());
function breakFinally() { var r = []; while (true) { try { break; } finally { r.push("fin"); } } return r; } print(breakFinally());
try { undefinedFunction(); } catch (e) { print(e instanceof ReferenceError, e.name); }
try { null.prop; } catch (e) { print(e instanceof TypeError); }
try { (void 0)(); } catch (e) { print(e.constructor.name); }
try { throw { custom: 1 }; } catch ({ custom }) { print("destructured catch", custom); }
try { throw "x"; } catch { print("optional binding"); }
var e2 = new TypeError("msg"); print(e2.name, e2.message, e2 instanceof Error, Object.prototype.toString.call(e2), String(e2));
var err = new Error("outer", { cause: "inner" }); print(err.cause, "cause" in new Error("x"));
for (var k in [7, 8]) out.push(typeof k); print(out.slice(-2));
var forOf = []; for (const x of [1, 2, 3]) forOf.push(x * x); print(forOf);
for (let [k, v] of Object.entries({ a: 1, b: 2 })) out.push(k + v); print(out.slice(-2));
var closures = []; for (const x of [1, 2]) closures.push(() => x); print(closures.map(f => f()));
var count = 0; for (;;) { if (++count > 5) break; } print(count);
var str = ""; for (var c of "héy") str += c + "."; print(str);
var iterCount = 0; var it = { [Symbol.iterator]() { return { i: 0, next() { iterCount++; return { done: this.i >= 3, value: this.i++ }; }, return() { out.push("closed"); return {}; } }; } };
for (var v of it) { if (v == 1) break; } print(iterCount, out[out.length - 1]);
var depth = 0; function rec(n) { try { return n > 0 ? rec(n - 1) : "bottom"; } finally { depth++; } } print(rec(100), depth);
label1: label2: for (var q = 0; q < 2; q++) { continue label1; } print(q);
var w = 0; while (true) { w++; if (w < 3) continue; break; } print(w);
print((() => { try { throw new RangeError("r"); } catch (e) { return e.name; } })());
var x = 0; do x++; while (x < 5) print(x);
switch (typeof null) { case "object": print("null is object"); break; }
function multiReturn(n) { if (n > 2) return "big"; else if (n > 0) return "small"; return "zero"; } print(multiReturn(3), multiReturn(1), multiReturn(0));
var t = 0; for (var i2 = 0, j2 = 10; i2 < j2; i2++, j2--) t++; print(t);
try { try { throw new Error("a"); } catch (e) { throw new Error("b"); } finally { out.push("cleanup"); } } catch (e) { print(e.message, out[out.length - 1]); }
