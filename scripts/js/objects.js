// Objects, properties, prototypes, Object.* and Reflect (27o)
var o = { b: 1, a: 2, 10: "x", 2: "y", [Symbol.iterator]: 0, "-1": "m", c: 3 };
print(Object.keys(o), Object.getOwnPropertyNames(o), Object.values(o).length, JSON.stringify(Object.entries(o)));
delete o.a; o.a = 5; print(Object.keys(o).join(), "a" in o, o.hasOwnProperty("b"), Object.hasOwn(o, "zz"));
var p = Object.create({ inherited: 1 }, { own: { value: 2, enumerable: true }, hidden: { value: 3 } });
print(p.inherited, p.own, p.hidden, Object.keys(p), JSON.stringify(p), Object.getPrototypeOf(Object.getPrototypeOf(p)) === Object.prototype);
for (var k in p) print("for-in", k);
var d = Object.getOwnPropertyDescriptor(p, "hidden"); print(d.value, d.writable, d.enumerable, d.configurable);
p.hidden = 99; print(p.hidden);
(function () { "use strict"; try { p.hidden = 1; } catch (e) { print("strict:", e.name); } })();
var g = { _v: 1, get v() { return this._v * 10; }, set v(x) { this._v = x; } };
g.v = 4; print(g.v, Object.getOwnPropertyDescriptor(g, "v").get.name, JSON.stringify(g));
Object.defineProperty(g, "ro", { value: 7, writable: false, enumerable: false, configurable: false });
print(g.ro, Object.keys(g)); try { Object.defineProperty(g, "ro", { value: 8 }); } catch (e) { print(e.name); }
var f = Object.freeze({ x: 1, nested: { y: 2 } }); f.x = 2; f.nested.y = 3; f.z = 1;
print(f.x, f.nested.y, f.z, Object.isFrozen(f), Object.isFrozen(f.nested), Object.isSealed(f), Object.isExtensible(f));
var s = Object.seal({ a: 1 }); s.a = 2; delete s.a; s.b = 1; print(s.a, s.b, Object.isSealed(s), Object.isFrozen(s));
var ne = Object.preventExtensions({ q: 1 }); ne.r = 2; print(ne.r, Object.isExtensible(ne), delete ne.q, ne.q);
var fa = Object.freeze([1, 2]); try { "use strict"; fa.push(3); } catch (e) { print("frozen push", e.name); } print(fa.length);
print(Object.assign({ a: 1 }, { b: 2 }, null, { a: 3 }), JSON.stringify(Object.assign({}, "ab", [9])));
print(JSON.stringify(Object.fromEntries([["a", 1], ["b", [2]]])), JSON.stringify(Object.fromEntries(new Map([["k", "v"]]))));
var proto = { greet() { return "hi " + this.name; } }; var child = Object.setPrototypeOf({ name: "c" }, proto);
print(child.greet(), proto.isPrototypeOf(child), Object.prototype.isPrototypeOf.call(proto, child), child instanceof Object);
var nullp = Object.create(null); nullp.x = 1; print(Object.keys(nullp), typeof nullp.toString, "x" in nullp);
print(Object.prototype.toString.call(null), Object.prototype.toString.call(1), Object.prototype.toString.call(""), Object.prototype.toString.call([]),
      Object.prototype.toString.call(function () {}), Object.prototype.toString.call(new Date(0)), Object.prototype.toString.call(/x/),
      Object.prototype.toString.call(Math), Object.prototype.toString.call(JSON), Object.prototype.toString.call(new Map()), Object.prototype.toString.call({ [Symbol.toStringTag]: "Custom" }));
var sym = Symbol("s"), sym2 = Symbol.for("reg"); var os = { [sym]: 1, regular: 2 };
print(Object.getOwnPropertySymbols(os).length, Object.keys(os), sym.toString(), sym.description, Symbol.keyFor(sym2), Symbol.for("reg") === sym2, Symbol("x") === Symbol("x"));
print(Reflect.ownKeys(os).length, Reflect.has(os, "regular"), Reflect.get(os, "regular"), Reflect.set(os, "n", 5), os.n, Reflect.getPrototypeOf(os) === Object.prototype);
print(Reflect.apply(Math.max, null, [1, 3, 2]), Reflect.construct(Date, [0]).getTime(), Reflect.defineProperty({}, "x", { value: 1 }), Reflect.deleteProperty(Object.freeze({ a: 1 }), "a"));
var counter = 0; var lazy = { get x() { counter++; return 1; } }; lazy.x; lazy.x; print(counter);
var obj = { a: 1, b: { c: [1, 2, { d: 3 }] } }; print(obj.b.c[2].d, obj?.b?.c?.[1], obj.zz?.yy, obj.b.c.length);
var key = "dyn"; var comp = { [key + 1]: 1, [`t${2}`]: 2, [1 + 1]: 3 }; print(JSON.stringify(comp));
var short = 5, me = { short, method() { return this.short; }, ["computed" + "M"]() { return 2; } }; print(me.short, me.method(), me.computedM(), me.method.name, me.computedM.name);
var arrows = { v: 1, f: () => typeof this, g() { return (() => this.v)(); } }; print(arrows.f(), arrows.g());
print(Object.entries({ x: 1, y: [2] }).map(([k, v]) => k + v).join(), Object.getOwnPropertyNames("ab"), Object.keys([4, 5]));
var proxy = { __proto__: { inherited: true }, own: 1 }; print(proxy.inherited, Object.keys(proxy), "__proto__" in {});
var ss = new String("ab"); print(Object.keys(ss), ss.length, ss[1], Object.getOwnPropertyNames(ss), typeof ss.valueOf());
print(Object.is(NaN, NaN), Object.is(0, -0), Object.is("a", "a"), Object.is({}, {}));
var gd = Object.getOwnPropertyDescriptors({ get a() { return 1; }, b: 2 }); print(typeof gd.a.get, gd.b.value, gd.b.writable);
var big = {}; for (var i = 0; i < 1000; i++) big["k" + i] = i; for (var i = 0; i < 1000; i += 2) delete big["k" + i];
print(Object.keys(big).length, big.k999, big.k998, Object.keys(big)[0], Object.keys(big)[499]);
var numk = {}; numk[3] = "c"; numk["b"] = 1; numk[1] = "a"; numk["a"] = 2; numk[2] = "b"; print(Object.keys(numk).join());
print(typeof Object.prototype.__proto__, ({}).__proto__ === Object.prototype, [].__proto__ === Array.prototype);
var x = {}; x.__proto__ = Array.prototype; print(x instanceof Array, Array.isArray(x), typeof x.push);
print(Object.groupBy([1, 2, 3, 4], v => v % 2 ? "odd" : "even").odd, JSON.stringify(Object.groupBy("abc", c => c)));
