// Builds compat/test/yaml-parse-corpus.json (not committed: it carries third
// party text) from real-world YAML: the frontmatter and heads of Claude Code's
// own markdown assets, and the in.yaml inputs of the yaml-test-suite.
//   node build-yaml-parse-corpus.mjs <markdown-dir> <yaml-test-suite-dir>
// The markdown dir holds the bundle's *.md assets (zstd ones decompressed).
import { readdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const [mdDir, ytsDir] = process.argv.slice(2);
const out = { frontmatter: [], markdownHead: [], yamlTestSuite: [] };

for (const f of readdirSync(mdDir).sort()) {
	if (!f.endsWith(".md")) continue;
	const text = readFileSync(join(mdDir, f), "utf8");
	// The frontmatter as a loader would cut it: between a leading `---` line and the next one.
	const m = /^---\r?\n([\s\S]*?)\r?\n---(?:\r?\n|$)/.exec(text);
	if (m) out.frontmatter.push(m[1]);
	out.markdownHead.push(text.slice(0, 1500));
}

const walk = (dir) => {
	for (const f of readdirSync(dir).sort()) {
		const p = join(dir, f);
		if (statSync(p).isDirectory()) walk(p);
		else if (f === "in.yaml") out.yamlTestSuite.push(readFileSync(p, "utf8"));
	}
};
walk(ytsDir);

writeFileSync(new URL("./yaml-parse-corpus.json", import.meta.url), JSON.stringify(out));
console.log("frontmatter:", out.frontmatter.length, "markdownHead:", out.markdownHead.length, "yamlTestSuite:", out.yamlTestSuite.length);
