// Array methods (27o)
var a = [5, 1, 4, 2, 3];
print(a.length, a.join("-"), a.toString(), String(a.slice(1, 3)), a.indexOf(4), a.includes(2), a.lastIndexOf(9));
print(a.map(x => x * 2), a.filter(x => x % 2), a.reduce((s, x) => s + x, 0), a.reduceRight((s, x) => s + x, ""));
print(a.some(x => x > 4), a.every(x => x > 0), a.find(x => x > 3), a.findIndex(x => x > 3), a.findLast(x => x < 3), a.findLastIndex(x => x > 9));
print([...a].sort(), [...a].sort((x, y) => y - x), a, a.toSorted(), a.toReversed(), a.with(0, 9), a.toSpliced(1, 2, "x"));
print([10, 9, 1, 100, 25].sort(), ["b", "a", "C", "B"].sort(), [3, undefined, 1, , 2].sort(), [true, false].sort());
var b = [1, 2, 3, 4, 5]; print(b.splice(1, 2), b, b.splice(-1), b, b.splice(1, 0, "a", "b"), b);
var c = [1, 2]; print(c.push(3, 4), c, c.pop(), c.shift(), c.unshift(0, -1), c);
print([1, [2, [3, [4]]]].flat(), [1, [2, [3, [4]]]].flat(Infinity), [1, 2].flatMap(x => [x, x * 10]), [[1], [2]].flat(0).length);
print(Array.from({ length: 3 }, (_, i) => i * i), Array.of(7, 8), Array.from(new Set([1, 1, 2])), Array.from("ab"));
print(new Array(3).length, new Array(3, 4), Array(2).join("x"), [, 1, , 2].length, [, 1].hasOwnProperty(0), 0 in [, 1]);
print([1, 2, 3].fill(0), [1, 2, 3, 4].fill(9, 1, 3), [1, 2, 3, 4, 5].copyWithin(0, 3), [1, 2, 3, 4, 5].copyWithin(1, 0, 2));
print([..."abc"].entries().next().value, [...["a", "b"].keys()], [...["a", "b"].values()], [...["a", "b"].entries()].join(";"));
print([1, 2, 3].reverse(), [1, 2, 3].at(-1), [].at(0), [0, -0, NaN].includes(NaN), [NaN].indexOf(NaN), [1, 2, 3].concat(4, [5, [6]]));
var arr = [1, 2, 3]; arr.length = 1; print(arr, arr.length); arr[5] = 6; print(arr.length, arr, arr[3]);
var sparse = []; sparse[10] = 1; print(sparse.length, sparse.join().length, Object.keys(sparse), sparse.indexOf(undefined));
var huge = []; huge[4294967294] = "last"; print(huge.length); huge.length = 0; print(huge.length, huge[4294967294]);
print(Array.isArray([]), Array.isArray({}), Array.isArray(Array.prototype), [] instanceof Array, typeof []);
print([1, 2, 3].join(), [null, undefined, 1].join("|"), [[1, 2], [3]].join(";"), [].join(), [1].join(undefined));
var cyc = [1, 2]; cyc.push(cyc); print(cyc.join(), String(cyc));
print([3, 1, 2].sort().map(String), ["10", "9", "1"].map(Number).sort((x, y) => x - y), [1, 2, 3, 4].filter((x, i, arr) => arr.length > i * 2));
var st = [{ k: 1, v: "a" }, { k: 0, v: "b" }, { k: 1, v: "c" }, { k: 0, v: "d" }];
print(st.sort((x, y) => x.k - y.k).map(o => o.v).join(""));
var objs = Array.from({ length: 50 }, (_, i) => ({ i, g: i % 3 }));
print(objs.sort((x, y) => x.g - y.g).map(o => o.i).join(","));
print([1, 2, 3, 4, 5].slice(-2), [1, 2, 3].slice(1, -1), [1, 2, 3].indexOf(2, -1), [1, 2, 1].lastIndexOf(1, -2));
var al = { length: 3, 0: "a", 1: "b", 2: "c" }; print(Array.prototype.join.call(al, "+"), Array.prototype.map.call(al, x => x.toUpperCase()));
print(Array.prototype.slice.call("abc"), [].concat.call(1, 2), Array.prototype.push.call(al, "d"), al.length, al[3]);
var e = []; e.forEach(x => print("never")); print(e.map(x => x).length, e.reduce((a, b) => a + b, "init"));
try { [].reduce((a, b) => a); } catch (er) { print(er.name); }
try { new Array(-1); } catch (er) { print(er.name); }
print([1, 2, 3].keys().toString(), Object.prototype.toString.call([].entries()), typeof [][Symbol.iterator]);
print([0, 1, 2].map((x, i, arr) => arr), [, "x"].map(v => v + 1), Array.from([1, 2], function (x) { return x + this.k; }, { k: 10 }));
var big = []; for (var i = 0; i < 10000; i++) big.push(i % 97); print(big.length, big.sort((x, y) => x - y)[5000], big.reduce((s, x) => s + x));
print([5, 1, 10].sort().toString(), [-1, -2, 3].sort().toString(), ["é", "e", "f"].sort().toString());
print(Array.prototype.concat.length, Array.prototype.push.length, Array.prototype.slice.length, Array.from.length);
