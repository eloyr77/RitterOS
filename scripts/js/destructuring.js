// Destructuring, spread, templates, optional chaining (27o)
var [a, b = 2, ...rest] = [1, undefined, 3, 4]; print(a, b, rest);
var { x, y: { z = 5 } = {}, ...others } = { x: 1, w: 2, v: 3 }; print(x, z, JSON.stringify(others));
var [, , third] = "abc"; print(third);
var [m, n] = new Set([7, 8]); print(m, n);
var { length } = "hello"; print(length);
var { [`dyn${1}`]: dyn } = { dyn1: "computed" }; print(dyn);
var p, q; [p, q] = [1, 2]; [p, q] = [q, p]; print(p, q);
var o = {}; ({ a: o.first, b: o["second"] } = { a: 1, b: 2 }); print(o.first, o.second);
var arr = []; [arr[0], arr[1]] = [5, 6]; print(arr);
function f({ a = 1, b } = {}, [c, ...d] = [9, 8, 7]) { return [a, b, c, d]; } print(JSON.stringify(f()), JSON.stringify(f({ b: 2 }, [1])));
print(Math.max(...[1, 5, 3]), [..."hi", ...[1, 2]], [...new Map([[1, 2]])].join(), String([...[, 1]]));
var merged = { ...{ a: 1, b: 2 }, b: 3, ...null, ...undefined, ..."xy" }; print(JSON.stringify(merged));
var deep = { l1: { l2: { l3: "deep" } } }; print(deep?.l1?.l2?.l3, deep?.nope?.l2, deep.l1?.["l2"].l3);
var fnObj = { method() { return "called"; } }; print(fnObj.method?.(), fnObj.missing?.(), fnObj?.missing?.());
var nul = null; print(nul?.a.b.c, nul?.[0], typeof nul?.x, (nul?.a)?.b);
print(`${1}${2}`, `a${[1, 2]}b`, `${{}}`, `${null} ${undefined}`, `\``, `$`, `${"$"}{}`);
function swap([a, b]) { return [b, a]; } print(swap([1, 2]));
var { a: aa, a: ab } = { a: "same" }; print(aa, ab);
try { var { u } = null; } catch (e) { print(e.name); }
try { var [v] = {}; } catch (e) { print(e.name); }
var [g = (() => "lazy")()] = []; print(g);
var count = 0; var [h = ++count] = [1]; print(h, count);
for (var [k, val] of [["a", 1], ["b", 2]]) print(k, val);
for (var { id, tags: [first] } of [{ id: 1, tags: ["t1", "t2"] }]) print(id, first);
var nestedArr = [[1, [2, 3]], [4]]; var [[n1, [, n3]], [n4]] = nestedArr; print(n1, n3, n4);
var str = "a,b"; var [s1, s2] = str.split(","); print(s1 + s2);
function spreadArgs(...args) { return args.length; } print(spreadArgs(...[1, 2], 3, ...[]), spreadArgs(..."abc"));
print(new Date(...[2020, 0, 2]).getDate(), Array.of(...[1, 2]).length);
var obj2 = { a: 1, get b() { return 2; } }; var copy = { ...obj2 }; print(Object.getOwnPropertyDescriptor(copy, "b").value);
var { toString: ts } = 1; print(typeof ts);
var [first2, ...[second2, third2]] = [1, 2, 3]; print(second2, third2);
