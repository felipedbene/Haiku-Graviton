// run.js <out.json>: run every scenario; record [name, result | {threw}] pairs.
const { writeFileSync } = require("fs");
const { S } = require("./scenarios.js");
(async () => {
	const res = [];
	for (const [k, f] of Object.entries(S)) {
		try { res.push([k, await f()]); } catch (e) { res.push([k, { threw: String(e && e.message) }]); }
	}
	writeFileSync(process.argv[2], JSON.stringify(res, null, 1));
	console.log("scenarios:", res.length);
	process.exit(0);
})();
