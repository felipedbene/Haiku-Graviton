// Process must exit with an unref'd idle worker; prints exit timing class.
const t0 = Date.now();
const w = new Worker(URL.createObjectURL(new Blob([`onmessage = () => {}; setInterval(() => {}, 1000)`])));
w.unref();
process.on("exit", () => console.log("exited", Date.now() - t0 < 3000 ? "fast" : "slow"));
