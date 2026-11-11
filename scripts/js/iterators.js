// Iterators, symbols, Map/Set/WeakMap (27o)
var m = new Map([[1, "a"], ["1", "b"], [NaN, "nan"], [-0, "zero"]]);
print(m.size, m.get(1), m.get("1"), m.get(NaN), m.get(0), m.has(-0), [...m.keys()].join(), [...m.values()].join());
m.set({}, "obj").delete("1"); print(m.size, [...m].length, m.get("1"));
var order = []; m.forEach((v, k) => order.push(String(k))); print(order);
var s = new Set([3, 1, 3, 2, 1]); print(s.size, [...s], s.has(2), s.add(4) === s, s.delete(1), [...s.entries()].join("|"));
var live = new Set([1, 2, 3]), seen = []; for (var v of live) { seen.push(v); if (v === 1) { live.delete(2); live.add(4); } } print(seen);
var mp = new Map(); mp.set("a", 1); var it = mp.entries(); mp.set("b", 2); print(JSON.stringify([...it]));
var cleared = new Map([[1, 1]]); var cit = cleared.keys(); cleared.clear(); cleared.set(2, 2); print([...cit]);
var u = new Set([1, 2, 3]), w = new Set([2, 3, 4]);
print([...u.union(w)], [...u.intersection(w)], [...u.difference(w)], [...u.symmetricDifference(w)], u.isSubsetOf(new Set([1, 2, 3, 5])), u.isSupersetOf(new Set([1])), u.isDisjointFrom(new Set([9])));
var wm = new WeakMap(), key = {}; wm.set(key, "val"); print(wm.get(key), wm.has({}), wm.delete(key), wm.has(key));
var ws = new WeakSet([key]); print(ws.has(key));
try { wm.set(1, 2); } catch (e) { print(e.name); }
print(Object.prototype.toString.call(m.entries()), typeof m[Symbol.iterator], m[Symbol.iterator] === m.entries, Set.prototype.keys === Set.prototype.values);
function* notSupported() {}
var custom = { from: 1, to: 4, [Symbol.iterator]() { var c = this.from, t = this.to; return { next: () => c <= t ? { value: c++, done: false } : { value: undefined, done: true } }; } };
print([...custom], Array.from(custom), Math.max(...custom));
var [c1, c2] = custom; print(c1, c2);
var ai = [1, 2][Symbol.iterator](); print(JSON.stringify(ai.next()), JSON.stringify(ai.next()), JSON.stringify(ai.next()));
var si = "ab"[Symbol.iterator](); print(si.next().value, si.next().value, si.next().done, si[Symbol.iterator]() === si);
var sym = Symbol("tag"), sym2 = Symbol(); print(typeof sym, sym.toString(), String(sym2), sym.description, sym2.description);
var symObj = { [sym]: 1, regular: 2 }; print(Object.keys(symObj), JSON.stringify(symObj), symObj[sym]);
print(Symbol.iterator.toString(), typeof Symbol.asyncIterator, Symbol.for("a") === Symbol.for("a"));
try { new Symbol(); } catch (e) { print(e.name); }
try { sym + ""; } catch (e) { print(e.name); }
var hasInst = { [Symbol.hasInstance](v) { return v === 1; } }; print(1 instanceof hasInst, 2 instanceof hasInst);
var prim = { [Symbol.toPrimitive](hint) { return hint === "number" ? 42 : hint === "string" ? "str" : "default"; } };
print(+prim, `${prim}`, prim + "", prim * 1);
var tagged = { [Symbol.toStringTag]: "Tagged" }; print(String(tagged));
var mapGroups = Map.groupBy([1, 2, 3, 4, 5], x => x % 2 ? "odd" : "even"); print(mapGroups.get("odd"), mapGroups.get("even"));
var entries = Object.entries({ a: 1, b: 2 }); var fromMap = new Map(entries); print(fromMap.get("b"), Object.fromEntries(fromMap).a);
var spreadable = { length: 2, 0: "x", 1: "y", [Symbol.isConcatSpreadable]: true }; print([1].concat(spreadable));
var big = new Map(); for (var i = 0; i < 2000; i++) big.set("k" + i, i); for (var i = 0; i < 2000; i += 2) big.delete("k" + i);
print(big.size, big.get("k1999"), [...big.keys()][0], [...big.values()].reduce((a, b) => a + b));
var setOfObjs = new Set(); var o1 = {}; setOfObjs.add(o1).add(o1).add({}); print(setOfObjs.size);
