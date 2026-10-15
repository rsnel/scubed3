/* scubed3 website: the passphrase demo, the paranoia simulator and the
 * charts; plain JavaScript, nothing is loaded from elsewhere */
"use strict";

(function () {

var reduced = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
var dpr = Math.max(1, Math.min(3, window.devicePixelRatio || 1));

function css(name) {
	return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

/* a small seeded PRNG (mulberry32), so the demo is the same every visit */
function prng(seed) {
	return function () {
		seed |= 0; seed = seed + 0x6D2B79F5 | 0;
		var t = Math.imul(seed ^ seed >>> 15, 1 | seed);
		t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t;
		return ((t ^ t >>> 14) >>> 0) / 4294967296;
	};
}

function shuffle(a, rnd) {
	for (var i = a.length - 1; i > 0; i--) {
		var j = Math.floor(rnd() * (i + 1)), t = a[i];
		a[i] = a[j]; a[j] = t;
	}
	return a;
}

function fmt(n) {	/* 1 024 with a thin space */
	return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, " ");
}

function roundRect(g, x, y, w, h, r) {
	g.beginPath();
	g.moveTo(x + r, y);
	g.arcTo(x + w, y, x + w, y + h, r);
	g.arcTo(x + w, y + h, x, y + h, r);
	g.arcTo(x, y + h, x, y, r);
	g.arcTo(x, y, x + w, y, r);
	g.closePath();
}

/* a canvas that is sized to its CSS width, with crisp pixels */
function fitCanvas(c, aspect) {
	var w = c.clientWidth || c.width;
	var h = Math.round(w * aspect);
	c.width = Math.round(w * dpr);
	c.height = Math.round(h * dpr);
	c.style.height = h + "px";
	var g = c.getContext("2d");
	g.setTransform(dpr, 0, 0, dpr, 0, 0);
	return { g: g, w: w, h: h };
}

var redrawers = [];
window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", function () {
	redrawers.forEach(function (f) { f(); });
});
var resizeTimer;
window.addEventListener("resize", function () {
	clearTimeout(resizeTimer);
	resizeTimer = setTimeout(function () { redrawers.forEach(function (f) { f(); }); }, 120);
});

/* ------------------------------------------------------------- tooltip */
var tip = document.getElementById("tip");
function showTip(html, x, y) {
	tip.innerHTML = html;
	tip.classList.add("on");
	var r = tip.getBoundingClientRect();
	var left = Math.min(window.innerWidth - r.width - 8, x + 14);
	var top = y - r.height - 12;
	if (top < 8) top = y + 18;
	tip.style.left = Math.max(8, left) + "px";
	tip.style.top = top + "px";
}
function hideTip() { tip.classList.remove("on"); }
window.addEventListener("scroll", hideTip, { passive: true });

/* ================================================== the passphrase demo */
(function () {
	var c = document.getElementById("store");
	if (!c) return;
	var COLS = 64, ROWS = 16, N = COLS * ROWS;
	var rnd = prng(3);
	var shade = [];
	for (var i = 0; i < N; i++) shade.push(Math.floor(rnd() * 3));
	var order = shuffle(Array.from({ length: N }, function (_, i) { return i; }), rnd);
	var parts = {
		"holiday photos": { blocks: order.slice(0, 140), color: "--a" },
		"tax papers": { blocks: order.slice(140, 200), color: "--b" },
		"diary": { blocks: order.slice(200, 236), color: "--c" }
	};
	var owner = new Array(N).fill(null);
	var shown = null, revealT = 0, flash = {};

	function draw() {
		var cols = c.clientWidth < 560 ? 32 : COLS;
		var rows = N / cols;
		var f = fitCanvas(c, rows / cols);
		var gap = f.w < 700 ? 1.5 : 2.5;
		var cw = (f.w - gap * (cols - 1)) / cols;
		var ch = (f.h - gap * (rows - 1)) / rows;
		var noise = [css("--noise-1"), css("--noise-2"), css("--noise-3")];
		var g = f.g;
		g.clearRect(0, 0, f.w, f.h);
		var now = performance.now();
		for (var i = 0; i < N; i++) {
			var x = (i % cols) * (cw + gap), y = Math.floor(i / cols) * (ch + gap);
			var col = noise[shade[i]];
			if (owner[i]) {
				var t = reduced ? 1 : Math.min(1, Math.max(0, (now - revealT - owner[i].delay) / 260));
				g.fillStyle = col;
				roundRect(g, x, y, cw, ch, Math.min(2, cw / 4));
				g.fill();
				g.globalAlpha = t;
				g.fillStyle = css(owner[i].color);
			} else {
				g.fillStyle = col;
			}
			roundRect(g, x, y, cw, ch, Math.min(2, cw / 4));
			g.fill();
			g.globalAlpha = 1;
			if (flash[i]) {
				var a = 1 - (now - flash[i]) / 900;
				if (a <= 0) delete flash[i];
				else {
					g.globalAlpha = a * 0.55;
					g.fillStyle = css("--paper");
					roundRect(g, x, y, cw, ch, Math.min(2, cw / 4));
					g.fill();
					g.globalAlpha = 1;
				}
			}
		}
	}
	redrawers.push(draw);

	var animating = false;
	function animate() {
		if (animating) return;
		animating = true;
		var end = performance.now() + 1400;
		(function step() {
			draw();
			if (performance.now() < end || Object.keys(flash).length) requestAnimationFrame(step);
			else animating = false;
		})();
	}

	var verdict = document.getElementById("verdict");
	var chips = document.querySelectorAll(".chip");
	var input = document.getElementById("pass");

	function open(pass) {
		var key = pass.trim().toLowerCase();
		owner.fill(null);
		chips.forEach(function (b) { b.setAttribute("aria-pressed", b.dataset.pass === key ? "true" : "false"); });
		if (!key) {
			shown = null;
			draw();
			return;
		}
		var p = parts[key];
		if (!p) {
			shown = null;
			verdict.innerHTML = "<b>No blocks found.</b> That is what scubed3 says when a passphrase is wrong, and also when there is simply nothing there. Nobody can tell the difference, and that is the point.";
			draw();
			return;
		}
		shown = key;
		/* the reveal: the blocks light up in a sweep from left to right */
		p.blocks.forEach(function (b) {
			owner[b] = { color: p.color, delay: (b % COLS) * 9 + Math.random() * 120 };
		});
		revealT = performance.now();
		var mib = p.blocks.length * 4;
		verdict.innerHTML = "<b>" + key + ": " + p.blocks.length + " blocks found</b>, " + fmt(mib) +
			"&#8239;MiB, scattered over the whole device. The other " + fmt(N - p.blocks.length) +
			" blocks still look like noise to this passphrase, including the ones of the other partitions.";
		if (reduced) draw(); else animate();
	}

	chips.forEach(function (b) {
		b.addEventListener("click", function () {
			var on = b.getAttribute("aria-pressed") === "true";
			input.value = on ? "" : b.dataset.pass;
			open(input.value);
		});
	});
	document.getElementById("unlock").addEventListener("submit", function (e) {
		e.preventDefault();
		open(input.value);
	});
	input.addEventListener("keydown", function (e) {
		if (e.key === "Enter") { e.preventDefault(); open(input.value); }
	});

	draw();

	/* level 3 at rest: now and then a block is rewritten, whether it holds
	 * anything or not */
	if (!reduced) {
		var visible = true;
		new IntersectionObserver(function (es) { visible = es[0].isIntersecting; }).observe(c);
		setInterval(function () {
			if (!visible || document.hidden) return;
			var b = Math.floor(Math.random() * N);
			if (!owner[b]) shade[b] = Math.floor(Math.random() * 3);
			flash[b] = performance.now();
			animate();
		}, 1100);
	}
})();

/* ===================================================== the paranoia sim */
(function () {
	var root = document.getElementById("sim");
	if (!root) return;
	var COLS = 12, ROWS = 8, N = COLS * ROWS;
	var own = document.getElementById("simown"), obs = document.getElementById("simobs");
	var stat = document.getElementById("simstat");
	var btns = root.querySelectorAll(".seg button");
	var rnd = prng(11);
	var order = shuffle(Array.from({ length: N }, function (_, i) { return i; }), rnd);
	var owner = new Array(N).fill(0);	/* 0 free, 1 yours, 2 another */
	order.slice(0, 24).forEach(function (b) { owner[b] = 1; });
	order.slice(24, 40).forEach(function (b) { owner[b] = 2; });
	var mine = order.slice(0, 24);
	var level = 1, heat, live, last, total, yours, data, pending, busy, clock;

	function reset() {
		heat = new Array(N).fill(0);
		live = new Array(N).fill(0);
		mine.forEach(function (b) { live[b] = 0.3 + 0.7 * rnd(); });
		last = -1; total = 0; yours = 0; data = 0; pending = 0; busy = false;
		draw(); report();
	}

	function grid(c, paint) {
		var f = fitCanvas(c, ROWS / COLS);
		var gap = 3, cw = (f.w - gap * (COLS - 1)) / COLS, ch = (f.h - gap * (ROWS - 1)) / ROWS;
		f.g.clearRect(0, 0, f.w, f.h);
		for (var i = 0; i < N; i++) {
			var x = (i % COLS) * (cw + gap), y = Math.floor(i / COLS) * (ch + gap);
			paint(f.g, i, x, y, cw, ch);
			if (i === last) {
				f.g.lineWidth = 2;
				f.g.strokeStyle = css("--ink");
				roundRect(f.g, x + 1, y + 1, cw - 2, ch - 2, 3);
				f.g.stroke();
			}
		}
	}

	function draw() {
		var col = [css("--noise-2"), css("--a"), css("--b")];
		grid(own, function (g, i, x, y, w, h) {
			g.fillStyle = col[owner[i]];
			roundRect(g, x, y, w, h, 3);
			g.fill();
		});
		var max = Math.max(4, Math.max.apply(null, heat));
		var base = css("--noise-1"), hc = css("--heat");
		grid(obs, function (g, i, x, y, w, h) {
			g.fillStyle = base;
			roundRect(g, x, y, w, h, 3);
			g.fill();
			if (heat[i]) {
				g.fillStyle = "rgba(" + hc + "," + (0.15 + 0.75 * heat[i] / max).toFixed(3) + ")";
				roundRect(g, x, y, w, h, 3);
				g.fill();
			}
		});
	}
	redrawers.push(draw);

	function report() {
		var s;
		if (level === 3) {
			s = "The clock has written " + total + " blocks, " + data + " of them with your data. ";
			s += pending ? pending + (pending === 1 ? " block waits" : " blocks wait") + " for the turn of your partition. " : "";
			s += total >= 8 ? "The writes are spread evenly and go on when you stop: the observer cannot tell when, or whether, you write." : "Writes happen on a random clock, also when you do nothing.";
		} else if (!total) {
			s = "Press the button to write a block. The counts start over when you change the level.";
		} else {
			var share = Math.round(100 * yours / total);
			if (level === 2) {
				s = total + (total === 1 ? " block written" : " blocks written") + " for " + data + (data === 1 ? " block" : " blocks") + " of your data. ";
				s += "Every write went to a random block of the whole device: yours, the other partition's, or free space. Over time every block is written about equally often, so the observer cannot tell which blocks are yours.";
				stat.textContent = s;
				return;
			}
			s = total + (total === 1 ? " block written" : " blocks written") + " for " + data + (data === 1 ? " block" : " blocks") + " of your data. " +
				share + "% of the writes landed on your partition, which is 25% of the device. ";
			if (total >= 4) s += "The observer sees exactly which blocks are a partition.";
		}
		stat.textContent = s;
	}

	function writeAt(b) {
		heat[b]++;
		total++;
		if (owner[b] === 1) yours++;
		last = b;
	}

	function pickLow() {	/* level 0: the emptiest block of the partition */
		var best = -1;
		mine.forEach(function (b) {
			if (b !== last && (best < 0 || live[b] < live[best])) best = b;
		});
		return best;
	}

	function partitionWrite(b) {	/* the old data elsewhere becomes garbage */
		live[b] = 1;
		var o = mine[Math.floor(Math.random() * mine.length)];
		if (o !== b) live[o] = Math.max(0, live[o] - 0.25 - 0.3 * Math.random());
	}

	function write() {
		if (level === 3) {
			pending++;
			report();
			return;
		}
		if (busy) return;
		data++;
		if (level === 0 || level === 1) {
			var b = level === 0 ? pickLow() : mine[Math.floor(Math.random() * mine.length)];
			if (level === 1) while (b === last) b = mine[Math.floor(Math.random() * mine.length)];
			partitionWrite(b);
			writeAt(b);
			draw(); report();
			return;
		}
		/* level 2: the scheduler draws who writes, until it is your partition;
		 * every draw is a write (other partitions rewrite their block, free
		 * blocks get new random data) */
		busy = true;
		(function draw1() {
			var b = Math.floor(Math.random() * N);
			if (owner[b] === 1) partitionWrite(b);
			writeAt(b);
			draw(); report();
			if (owner[b] === 1) { busy = false; return; }
			setTimeout(draw1, reduced ? 0 : 110);
		})();
	}

	function tick() {
		var b = Math.floor(Math.random() * N);
		if (owner[b] === 1) {
			partitionWrite(b);
			if (pending) { pending--; data++; }
		}
		writeAt(b);
		draw(); report();
	}

	var visible = false;
	new IntersectionObserver(function (es) { visible = es[0].isIntersecting; }).observe(root);
	function setLevel(l) {
		level = l;
		btns.forEach(function (b) { b.setAttribute("aria-pressed", +b.dataset.level === l ? "true" : "false"); });
		clearInterval(clock);
		if (l === 3) clock = setInterval(function () { if (visible && !document.hidden) tick(); }, 420);
		reset();
	}
	btns.forEach(function (b) { b.addEventListener("click", function () { setLevel(+b.dataset.level); }); });
	document.getElementById("simwrite").addEventListener("click", write);
	document.getElementById("simreset").addEventListener("click", reset);
	setLevel(1);
})();

/* ============================================================== charts */
var NS = "http://www.w3.org/2000/svg";
function el(name, attrs, parent, text) {
	var e = document.createElementNS(NS, name);
	for (var k in attrs) e.setAttribute(k, attrs[k]);
	if (text !== undefined) e.textContent = text;
	if (parent) parent.appendChild(e);
	return e;
}
function hover(target, html) {
	target.addEventListener("pointermove", function (e) { showTip(html(e), e.clientX, e.clientY); });
	target.addEventListener("pointerleave", hideTip);
	target.addEventListener("pointerdown", function (e) { showTip(html(e), e.clientX, e.clientY); });
}
function svgBox(host, w, h, label) {
	host.textContent = "";
	return el("svg", { viewBox: "0 0 " + w + " " + h, role: "img", "aria-label": label }, host);
}
/* a horizontal bar, rounded at the data end (4px), square at the baseline */
function hbar(s, x0, y, len, h, cls, color) {
	var r = Math.min(4, len / 2, h / 2);
	var d = "M" + x0 + " " + y + "H" + (x0 + len - r) + "A" + r + " " + r + " 0 0 1 " + (x0 + len) + " " + (y + r) +
		"V" + (y + h - r) + "A" + r + " " + r + " 0 0 1 " + (x0 + len - r) + " " + (y + h) + "H" + x0 + "Z";
	return el("path", { d: d, style: "fill:" + color }, s);
}

/* --- open time: two small multiples, each on its own linear scale */
(function () {
	var host = document.getElementById("chart-open");
	if (!host) return;
	var panels = [
		{ title: "First open after the start", max: 2, ticks: [0, 0.5, 1, 1.5, 2], rows: [["before", 1.77], ["now", 0.145]] },
		{ title: "Every later open", max: 0.16, ticks: [0, 0.04, 0.08, 0.12, 0.16], rows: [["before", 0.151], ["now", 0.008]] }
	];
	function draw() {
		var W = 440, L = 64, R = 56, PH = 116, H = PH * 2 + 6;
		var s = svgBox(host, W, H, "Open time in seconds: first open 1.77 before, 0.145 now; later opens 0.151 before, 0.008 now");
		panels.forEach(function (p, k) {
			var y0 = k * (PH + 6);
			var sx = function (v) { return L + v / p.max * (W - L - R); };
			el("text", { x: 0, y: y0 + 14, "class": "lbl", "font-weight": "700" }, s, p.title);
			p.ticks.forEach(function (t) {
				el("line", { x1: sx(t), x2: sx(t), y1: y0 + 26, y2: y0 + 86, "class": t ? "grid" : "axis" }, s);
				el("text", { x: sx(t), y: y0 + 102, "text-anchor": "middle" }, s, (t === 0 ? "0" : String(t)) + (t === p.max ? " s" : ""));
			});
			p.rows.forEach(function (r, j) {
				var y = y0 + 32 + j * 28, len = Math.max(1.5, sx(r[1]) - L);
				el("text", { x: L - 10, y: y + 15, "text-anchor": "end", "class": "lbl" }, s, r[0]);
				hbar(s, L, y, len, 20, "", j ? css("--a") : css("--ink-3"));
				el("text", { x: L + len + 6, y: y + 15, "class": "val" }, s, String(r[1]).replace(".", ".") + " s");
				var hit = el("rect", { x: 0, y: y - 2, width: W, height: 24, "class": "hit" }, s);
				hover(hit, function () {
					var other = p.rows[1 - j][1];
					return "<b>" + p.title + ", " + r[0] + "</b><br>" + r[1] + " s" +
						(j ? " (" + Math.round(other / r[1]) + " times faster)" : "");
				});
			});
		});
	}
	redrawers.push(draw);
	draw();
})();

/* --- read latency: a range dot plot (median to maximum), log scale */
(function () {
	var host = document.getElementById("chart-read");
	if (!host) return;
	var rows = [["before", 312, 3447], ["now", 0.2, 14]];
	function draw() {
		var W = 440, L = 64, R = 24, H = 196;
		var s = svgBox(host, W, H, "Read latency in milliseconds: before median 312, maximum 3447; now median 0.2, maximum 14");
		var lo = Math.log10(0.1), hi = Math.log10(10000);
		var sx = function (v) { return L + (Math.log10(v) - lo) / (hi - lo) * (W - L - R); };
		[0.1, 1, 10, 100, 1000, 10000].forEach(function (t) {
			el("line", { x1: sx(t), x2: sx(t), y1: 30, y2: 130, "class": "grid" }, s);
			el("text", { x: sx(t), y: 148, "text-anchor": "middle" }, s, (t >= 1000 ? fmt(t) : String(t)) + (t === 10000 ? " ms" : ""));
		});
		var surf = css("--paper");
		rows.forEach(function (r, j) {
			var y = 56 + j * 50, c = j ? css("--a") : css("--ink-3");
			el("text", { x: L - 12, y: y + 5, "text-anchor": "end", "class": "lbl" }, s, r[0]);
			el("line", { x1: sx(r[1]), x2: sx(r[2]), y1: y, y2: y, style: "stroke:" + c + ";stroke-width:2" }, s);
			el("circle", { cx: sx(r[1]), cy: y, r: 6, style: "fill:" + c + ";stroke:" + surf + ";stroke-width:2" }, s);
			el("circle", { cx: sx(r[2]), cy: y, r: 6, style: "fill:" + surf + ";stroke:" + c + ";stroke-width:2" }, s);
			el("text", { x: sx(r[1]), y: y - 13, "text-anchor": "middle", "class": "val" }, s, fmt(r[1]));
			el("text", { x: sx(r[2]), y: y - 13, "text-anchor": "middle", "class": "val" }, s, fmt(r[2]));
			var hit = el("rect", { x: 0, y: y - 22, width: W, height: 40, "class": "hit" }, s);
			hover(hit, function () { return "<b>" + r[0] + "</b><br>median " + r[1] + " ms<br>maximum " + fmt(r[2]) + " ms"; });
		});
		/* the legend: filled = median, open = maximum */
		var ly = 180;
		el("circle", { cx: L + 6, cy: ly - 4, r: 5, style: "fill:" + css("--ink-2") }, s);
		el("text", { x: L + 18, y: ly }, s, "median");
		el("circle", { cx: L + 92, cy: ly - 4, r: 5, style: "fill:" + surf + ";stroke:" + css("--ink-2") + ";stroke-width:2" }, s);
		el("text", { x: L + 104, y: ly }, s, "maximum");
	}
	redrawers.push(draw);
	draw();
})();

/* --- level 3 throughput: max KiB/s against the partition's share */
(function () {
	var host = document.getElementById("chart-rate");
	if (!host) return;
	var rates = [128, 256, 400, 800, 1600, 4096];
	var slider = document.getElementById("rate"), out = document.getElementById("rateout"), wear = document.getElementById("wear");
	function human(sec) {
		if (sec < 90) return Math.round(sec) + " seconds";
		if (sec < 5400) return Math.round(sec / 60) + " minutes";
		if (sec < 172800) return (sec / 3600).toFixed(1).replace(/\.0$/, "") + " hours";
		return Math.round(sec / 86400) + " days";
	}
	function draw() {
		var rate = rates[+slider.value];
		out.textContent = fmt(rate) + " KiB/s";
		var gb = rate * 1024 * 86400 / 1e9;
		wear.textContent = "the device is written " + (gb < 100 ? gb.toFixed(1) : Math.round(gb)) + " GB a day" + (rate === 800 ? " (the default)" : "");
		var W = 900, L = 64, R = 24, T = 34, B = 44, H = 300;
		var s = svgBox(host, W, H, "Maximum write speed of a partition in level 3: its share of the device times the rate, " + rate + " KiB/s");
		var ymax = 4096;
		var sx = function (f) { return L + f * (W - L - R); };
		var sy = function (v) { return T + (1 - v / ymax) * (H - T - B); };
		[0, 1024, 2048, 3072, 4096].forEach(function (t) {
			el("line", { x1: L, x2: W - R, y1: sy(t), y2: sy(t), "class": t ? "grid" : "axis" }, s);
			el("text", { x: L - 10, y: sy(t) + 4, "text-anchor": "end" }, s, fmt(t));
		});
		el("text", { x: L - 10, y: T - 16, "text-anchor": "end" }, s, "KiB/s");
		[0, 0.25, 0.5, 0.75, 1].forEach(function (f) {
			el("text", { x: sx(f), y: H - B + 20, "text-anchor": "middle" }, s, Math.round(f * 100) + "%");
		});
		el("text", { x: sx(1), y: H - 6, "text-anchor": "end" }, s, "share of the device that the partition has");
		var a = css("--a"), surf = css("--paper");
		el("line", { x1: sx(0), y1: sy(0), x2: sx(1), y2: sy(rate), style: "stroke:" + a + ";stroke-width:2" }, s);
		[0.1, 0.25, 0.5].forEach(function (f) {
			el("circle", { cx: sx(f), cy: sy(f * rate), r: 5, style: "fill:" + a + ";stroke:" + surf + ";stroke-width:2" }, s);
			el("text", { x: sx(f) + 2, y: sy(f * rate) - 12, "text-anchor": "middle", "class": "val" }, s, fmt(Math.round(f * rate)));
		});
		var cross = el("line", { x1: 0, x2: 0, y1: T, y2: H - B, "class": "axis", "stroke-dasharray": "3 3", visibility: "hidden" }, s);
		var dot = el("circle", { r: 5, style: "fill:" + a + ";stroke:" + surf + ";stroke-width:2", visibility: "hidden" }, s);
		var hit = el("rect", { x: L, y: T, width: W - L - R, height: H - T - B, "class": "hit" }, s);
		function at(e) {
			var p = s.createSVGPoint(); p.x = e.clientX; p.y = e.clientY;
			var q = p.matrixTransform(s.getScreenCTM().inverse());
			var f = Math.max(0.01, Math.min(1, Math.round((q.x - L) / (W - L - R) * 100) / 100));
			cross.setAttribute("x1", sx(f)); cross.setAttribute("x2", sx(f));
			dot.setAttribute("cx", sx(f)); dot.setAttribute("cy", sy(f * rate));
			cross.setAttribute("visibility", "visible"); dot.setAttribute("visibility", "visible");
			var kib = f * rate;
			return "<b>" + Math.round(f * 100) + "% of the blocks</b><br>at most " + fmt(Math.round(kib)) + " KiB/s<br>1 GB takes at least " + human(1e9 / (kib * 1024));
		}
		hover(hit, at);
		hit.addEventListener("pointerleave", function () { cross.setAttribute("visibility", "hidden"); dot.setAttribute("visibility", "hidden"); });
	}
	slider.addEventListener("input", draw);
	redrawers.push(draw);
	draw();
})();

})();
