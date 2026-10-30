// Shared by Bun and Node: the same Bun.which calls on the same fixture tree.
export const CASES = [["sh"], ["bash"], ["nonexistent-xyz"], ["tool", { PATH: "/tmp/wt/bin" }], ["noexec", { PATH: "/tmp/wt/bin" }],
	["dirtool", { PATH: "/tmp/wt/bin" }], ["link", { PATH: "/tmp/wt/bin" }], ["dangling", { PATH: "/tmp/wt/bin" }], ["tool", { PATH: "" }],
	["tool", { PATH: "/nope::/tmp/wt/bin" }], ["tool", { PATH: ":/tmp/wt/bin:" }], ["/tmp/wt/bin/tool"], ["/tmp/wt/bin/noexec"], ["/tmp/wt/bin/dirtool"],
	["./sub/rel"], ["sub/rel"], ["./sub/rel", { cwd: "/tmp/wt" }], ["./sub/rel", { cwd: "/tmp/wt///" }], ["rel", { cwd: "/tmp/wt/sub" }],
	["rel", { PATH: ".", cwd: "/tmp/wt/sub" }], ["rel", { PATH: "sub", cwd: "/tmp/wt" }], ["rel", { PATH: "sub", cwd: "relative" }],
	["sub/rel", { cwd: "" }], [""], [null], [undefined], ["tool", { PATH: "/tmp/wt/bin/" }], ["sh", {}], ["sh", null], ["sh", 5], ["sh", "str"],
	["tool", { PATH: "/tmp/wt/bin", cwd: 7 }], ["tool", { path: "/tmp/wt/bin" }], [123, { PATH: "/tmp/wt/bin" }], ["../wt/bin/tool", { cwd: "/tmp/wt" }],
	["tool", { PATH: "../wt/bin", cwd: "/tmp/wt" }], ["x".repeat(5000)], ["sp ace", { PATH: "/tmp/wt/bin" }]];
